#!/usr/bin/env python3
"""Three-way comparison driver for the TranslAPA baseline (design doc B4).

Compares, over the bc14 corpus and on the SAME elimination front-end
(path-expression DAG), three interpreters of that DAG:

  * default    -- framework generic interpreter (SolverContext::eval:
                  transfer re-application, Star iterated to a fixpoint)
  * ean        -- our equality-saturation DAG optimizer, then generic eval
  * translapa  -- the paper's closed-form Gen/Kill semiring fold (external
                  strong baseline; Star is O(1), no iteration)

Reported per client (reaching_defs, reachable):
  * DAG node count      -- unique hash-consed nodes (default == translapa,
                           since translapa does not rewrite the DAG; EAN reduces)
  * evaluation time     -- interp_us (generic vs generic-on-EAN vs closed-form fold)
  * one-time cost       -- EAN normalization vs TranslAPA mechanical extraction
                           (both recorded in norm_us)
  * correctness         -- IN facts: default vs translapa unequal lines (expect 0,
                           by the paper's correctness-by-construction guarantee)

This reuses real_eval.py's parser/cache so `run` is resumable and `agg`
re-runnable. Tags are prefixed `td_` to avoid clashing with the EAN harness.

Usage:
  translapa_eval.py run [--limit N] [--suite coreutils|open|spec] [--workers W]
  translapa_eval.py agg [--suite ...]
"""
import os, sys, subprocess, argparse, statistics
from concurrent.futures import ThreadPoolExecutor, as_completed

import real_eval as RE  # reuse TOOL/BC_ROOT/parse/programs/raw_path/geomean/...

CLIENTS = ["reaching_defs", "reachable"]
CAP = 300
TIMEOUT = 180
REPEAT = 3
CONFIGS = ["td_default", "td_ean", "td_translapa", "td_ean_translapa"]


def invoke(bc, client, config, outpath):
    if RE.done(outpath):
        return "cached"
    cmd = [RE.TOOL, bc, "--stdout", "--elim-method=state",
           f"--analysis={client}", "--ordering=default",
           f"--max-func-insts={CAP}", f"--repeat={REPEAT}", "--dump-profile"]
    if config == "td_ean":
        cmd += ["--ean", "--ean-laws=safe"] + RE.EAN_BUDGET
    elif config == "td_translapa":
        cmd += ["--interp=translapa"]
    elif config == "td_ean_translapa":
        # Composition: EAN shrinks the DAG (safe laws), then the closed-form
        # semiring folds the reduced DAG. The client sets InterpMemo internally
        # so the post-pass skips its discarded generic eval.
        cmd += ["--ean", "--ean-laws=safe", "--interp=translapa"] + RE.EAN_BUDGET
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT)
        txt = r.stdout
        if "[mem] peak_rss_kb=" not in txt:
            txt += f"\n[CRASH rc={r.returncode}]\n"
    except subprocess.TimeoutExpired as e:
        partial = e.stdout or e.output or ""
        if isinstance(partial, bytes):
            partial = partial.decode("utf-8", "ignore")
        txt = partial + f"\n[TIMEOUT] {TIMEOUT}s\n"
    with open(outpath, "w", encoding="utf-8") as f:
        f.write(txt)
    return "ran"


def process(suite, prog, bc):
    for client in CLIENTS:
        for config in CONFIGS:
            invoke(bc, client, config,
                   RE.raw_path(suite, prog, f"{config}_{client}"))
    return f"{suite}/{prog}"


def run(args):
    progs = RE.programs(args.suite)
    if args.limit:
        progs = progs[:args.limit]
    total = len(progs)
    workers = args.workers or RE.WORKERS
    sys.stderr.write(f"[translapa] running {total} programs, {workers} workers\n")
    done = 0
    with ThreadPoolExecutor(max_workers=workers) as ex:
        futs = {ex.submit(process, s, p, bc): (s, p) for (s, p, bc) in progs}
        for fut in as_completed(futs):
            s, p = futs[fut]
            done += 1
            try:
                fut.result()
            except Exception as e:  # noqa
                sys.stderr.write(f"  ERROR {s}/{p}: {e}\n")
            sys.stderr.write(f"[{done}/{total}] {s}/{p}\n")
    sys.stderr.write("[translapa] run complete\n")


def client_funcs(res, client):
    out = {}
    for fn, cl in res["funcs"].items():
        if client in cl and "nodes" in cl[client]:
            out[fn] = cl[client]
    return out


def agg(args):
    progs = RE.programs(args.suite)
    rows = []  # summary rows
    compose_rows = []  # 4-way composition rows (EAN ⊕ TranslAPA)
    for client in CLIENTS:
        # per-function ratios vs td_default
        variants = ["td_ean", "td_translapa", "td_ean_translapa"]
        r = {c: {"nodes": [], "interp": [], "end2end": []} for c in variants}
        oneshot = {"td_ean_norm": [], "td_translapa_extract": [],
                   "td_ean_translapa_norm": []}
        base_interp, ta_interp = [], []  # absolute medians (us)
        # Multiplicativity: fold-on-EAN-DAG / fold-on-raw-DAG, per function.
        # If TranslAPA's fold cost is ∝ unique nodes (memoized fold), this ratio
        # should track the EAN node-reduction ratio — the two levers multiply.
        mult_fold = []          # ean_translapa.interp / translapa.interp
        ean_node_ratio = []     # ean.nodes / default.nodes (paired w/ mult_fold)
        unequal_total = compared_total = nfun = 0
        for suite, prog, bc in progs:
            P = {c: RE.parse(RE.raw_path(suite, prog, f"{c}_{client}"))
                 for c in CONFIGS}
            base = client_funcs(P["td_default"], client)
            cf = {c: client_funcs(P[c], client) for c in variants}
            for fn, b in base.items():
                if b.get("nodes", 0) <= 0:
                    continue
                nfun += 1
                if "interp" in b:
                    base_interp.append(b["interp"])
                for c in variants:
                    cd = cf[c].get(fn)
                    if not cd or "nodes" not in cd:
                        continue
                    for k in ["nodes", "interp", "end2end"]:
                        bv, cv = b.get(k, 0), cd.get(k, 0)
                        if bv > 0 and cv > 0:
                            r[c][k].append(cv / bv)
                ta = cf["td_translapa"].get(fn)
                et = cf["td_ean_translapa"].get(fn)
                ea = cf["td_ean"].get(fn)
                if ta and "interp" in ta:
                    ta_interp.append(ta["interp"])
                # paired fold-on-ean vs fold-on-raw + the node ratio it should track
                if (ta and et and ta.get("interp", 0) > 0
                        and et.get("interp", 0) > 0):
                    mult_fold.append(et["interp"] / ta["interp"])
                    if ea and ea.get("nodes", 0) > 0 and b.get("nodes", 0) > 0:
                        ean_node_ratio.append(ea["nodes"] / b["nodes"])
                if ea and ea.get("norm", 0) > 0:
                    oneshot["td_ean_norm"].append(ea["norm"])
                if ta and ta.get("norm", 0) > 0:
                    oneshot["td_translapa_extract"].append(ta["norm"])
                if et and et.get("norm", 0) > 0:
                    oneshot["td_ean_translapa_norm"].append(et["norm"])
            # correctness: default vs {translapa, ean_translapa} IN facts
            dp = RE.raw_path(suite, prog, f"td_default_{client}")
            tp = RE.raw_path(suite, prog, f"td_ean_translapa_{client}")
            if not (os.path.exists(dp) and os.path.exists(tp)):
                continue
            if RE.is_incomplete(dp) or RE.is_incomplete(tp):
                continue
            dd = RE.in_by_func(dp, client)
            tt = RE.in_by_func(tp, client)
            for fn, a in dd.items():
                b = tt.get(fn)
                if not a or not b:
                    continue
                compared_total += min(len(a), len(b))
                for x, y in zip(a, b):
                    if x != y:
                        unequal_total += 1
                unequal_total += abs(len(a) - len(b))
        rows.append(dict(
            client=client, funcs=nfun,
            ean_nodes=RE.geomean(r["td_ean"]["nodes"]),
            translapa_nodes=RE.geomean(r["td_translapa"]["nodes"]),
            ean_interp=RE.geomean(r["td_ean"]["interp"]),
            translapa_interp=RE.geomean(r["td_translapa"]["interp"]),
            ean_e2e=RE.geomean(r["td_ean"]["end2end"]),
            translapa_e2e=RE.geomean(r["td_translapa"]["end2end"]),
            base_interp_us=statistics.median(base_interp) if base_interp else 0,
            translapa_interp_us=statistics.median(ta_interp) if ta_interp else 0,
            ean_norm_us=statistics.median(oneshot["td_ean_norm"]) if oneshot["td_ean_norm"] else 0,
            translapa_extract_us=statistics.median(oneshot["td_translapa_extract"]) if oneshot["td_translapa_extract"] else 0,
            compared=compared_total, unequal=unequal_total))
        compose_rows.append(dict(
            client=client,
            # nodes: EAN and EAN+TranslAPA share the reduced DAG (≈ equal);
            # TranslAPA alone keeps the raw DAG (≈ 1.00).
            ean_nodes=RE.geomean(r["td_ean"]["nodes"]),
            translapa_nodes=RE.geomean(r["td_translapa"]["nodes"]),
            ean_translapa_nodes=RE.geomean(r["td_ean_translapa"]["nodes"]),
            # evaluation time vs Default (lower is better).
            ean_interp=RE.geomean(r["td_ean"]["interp"]),
            translapa_interp=RE.geomean(r["td_translapa"]["interp"]),
            ean_translapa_interp=RE.geomean(r["td_ean_translapa"]["interp"]),
            # the multiplicativity check.
            fold_ean_over_raw=RE.geomean(mult_fold),
            ean_node_ratio=RE.geomean(ean_node_ratio),
            n_mult=len(mult_fold),
            ean_translapa_norm_us=statistics.median(oneshot["td_ean_translapa_norm"]) if oneshot["td_ean_translapa_norm"] else 0,
            unequal=unequal_total, compared=compared_total))

    out = os.path.join(RE.OUT, "translapa_compare.csv")
    with open(out, "w") as f:
        f.write("client,functions,ean_nodes_ratio,translapa_nodes_ratio,"
                "ean_interp_ratio,translapa_interp_ratio,ean_end2end_ratio,"
                "translapa_end2end_ratio,default_interp_us_median,"
                "translapa_interp_us_median,ean_norm_us_median,"
                "translapa_extract_us_median,in_lines_compared,unequal_lines\n")
        for r in rows:
            f.write(f"{r['client']},{r['funcs']},{r['ean_nodes']:.4f},"
                    f"{r['translapa_nodes']:.4f},{r['ean_interp']:.4f},"
                    f"{r['translapa_interp']:.4f},{r['ean_e2e']:.4f},"
                    f"{r['translapa_e2e']:.4f},{r['base_interp_us']:.0f},"
                    f"{r['translapa_interp_us']:.0f},{r['ean_norm_us']:.0f},"
                    f"{r['translapa_extract_us']:.0f},{r['compared']},{r['unequal']}\n")
    sys.stderr.write(f"[translapa] wrote {out}\n")

    # 4-way composition table (EAN ⊕ TranslAPA), including the multiplicativity
    # check: fold_ean_over_raw should track ean_node_ratio if EAN's smaller DAG
    # transfers linearly into the closed-form fold time.
    compose = os.path.join(RE.OUT, "translapa_compose.csv")
    with open(compose, "w") as f:
        f.write("client,ean_nodes_ratio,translapa_nodes_ratio,"
                "ean_translapa_nodes_ratio,ean_interp_ratio,"
                "translapa_interp_ratio,ean_translapa_interp_ratio,"
                "fold_ean_over_raw_ratio,ean_node_ratio_paired,n_paired,"
                "ean_translapa_norm_us_median,unequal_lines,in_lines_compared\n")
        for c in compose_rows:
            f.write(f"{c['client']},{c['ean_nodes']:.4f},"
                    f"{c['translapa_nodes']:.4f},{c['ean_translapa_nodes']:.4f},"
                    f"{c['ean_interp']:.4f},{c['translapa_interp']:.4f},"
                    f"{c['ean_translapa_interp']:.4f},"
                    f"{c['fold_ean_over_raw']:.4f},{c['ean_node_ratio']:.4f},"
                    f"{c['n_mult']},{c['ean_translapa_norm_us']:.0f},"
                    f"{c['unequal']},{c['compared']}\n")
    sys.stderr.write(f"[translapa] wrote {compose}\n")
    # human-readable echo
    for r in rows:
        sys.stderr.write(
            f"  {r['client']}: funcs={r['funcs']} "
            f"nodes(ean={r['ean_nodes']:.3f},ta={r['translapa_nodes']:.3f}) "
            f"interp(ean={r['ean_interp']:.3f},ta={r['translapa_interp']:.3f}) "
            f"unequal={r['unequal']}/{r['compared']}\n")
    for c in compose_rows:
        sys.stderr.write(
            f"  [compose] {c['client']}: "
            f"interp(ean={c['ean_interp']:.3f},ta={c['translapa_interp']:.3f},"
            f"ean+ta={c['ean_translapa_interp']:.3f}) "
            f"fold_ean/raw={c['fold_ean_over_raw']:.3f} "
            f"vs node_ratio={c['ean_node_ratio']:.3f} (n={c['n_mult']}) "
            f"unequal={c['unequal']}/{c['compared']}\n")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    rp = sub.add_parser("run")
    rp.add_argument("--limit", type=int, default=0)
    rp.add_argument("--suite", default="coreutils")
    rp.add_argument("--workers", type=int, default=0)
    ap_ = sub.add_parser("agg")
    ap_.add_argument("--suite", default="coreutils")
    a = ap.parse_args()
    run(a) if a.cmd == "run" else agg(a)

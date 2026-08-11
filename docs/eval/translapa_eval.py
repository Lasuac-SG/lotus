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
CONFIGS = ["td_default", "td_ean", "td_translapa"]


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
    for client in CLIENTS:
        # per-function ratios vs td_default
        r = {c: {"nodes": [], "interp": [], "end2end": []} for c in
             ["td_ean", "td_translapa"]}
        oneshot = {"td_ean_norm": [], "td_translapa_extract": []}
        base_interp, ta_interp = [], []  # absolute medians (us)
        unequal_total = compared_total = nfun = 0
        for suite, prog, bc in progs:
            P = {c: RE.parse(RE.raw_path(suite, prog, f"{c}_{client}"))
                 for c in CONFIGS}
            base = client_funcs(P["td_default"], client)
            ean = client_funcs(P["td_ean"], client)
            ta = client_funcs(P["td_translapa"], client)
            for fn, b in base.items():
                if b.get("nodes", 0) <= 0:
                    continue
                nfun += 1
                if "interp" in b:
                    base_interp.append(b["interp"])
                for c, cd in [("td_ean", ean.get(fn)), ("td_translapa", ta.get(fn))]:
                    if not cd or "nodes" not in cd:
                        continue
                    for k in ["nodes", "interp", "end2end"]:
                        bv, cv = b.get(k, 0), cd.get(k, 0)
                        if bv > 0 and cv > 0:
                            r[c][k].append(cv / bv)
                if fn in ta and "interp" in ta[fn]:
                    ta_interp.append(ta[fn]["interp"])
                if fn in ean and ean[fn].get("norm", 0) > 0:
                    oneshot["td_ean_norm"].append(ean[fn]["norm"])
                if fn in ta and ta[fn].get("norm", 0) > 0:
                    oneshot["td_translapa_extract"].append(ta[fn]["norm"])
            # correctness: default vs translapa IN facts
            dp = RE.raw_path(suite, prog, f"td_default_{client}")
            tp = RE.raw_path(suite, prog, f"td_translapa_{client}")
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
    # human-readable echo
    for r in rows:
        sys.stderr.write(
            f"  {r['client']}: funcs={r['funcs']} "
            f"nodes(ean={r['ean_nodes']:.3f},ta={r['translapa_nodes']:.3f}) "
            f"interp(ean={r['ean_interp']:.3f},ta={r['translapa_interp']:.3f}) "
            f"unequal={r['unequal']}/{r['compared']}\n")


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

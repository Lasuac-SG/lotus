#!/usr/bin/env python3
"""Affine-equalities EAN corpus driver (paper R1 positive case + memo break-even).

Runs the affine intra client with the MEMOIZING interpreter (--memo-interp) over
the bc14 corpus, in three configs:
  aff_def    : Default (memo)
  aff_ean    : EAN safe-minimal  (memo, monotone)
  aff_kleene : EAN full Kleene    (memo, monotone)

The affine framework is distributive, so full Kleene should be SOUND (0 unequal
facts vs Default) — unlike reachable/liveness. We also measure whether EAN's
node reduction lowers memo interpretation time (the RQ2 amortization direction).

The memoizing interpreter makes affine eval feasible on small/medium real
functions (cost proportional to unique DAG nodes, not the expanded tree); large
functions are capped/timed-out and reported honestly.

Usage:
  affine_eval.py run [--limit N] [--suite coreutils|open|spec]
  affine_eval.py agg

Reuses real_eval.py's tool runner/parser/comparators; raw outputs cached under
raw/ so `run` is resumable and `agg` re-runnable.
"""
import os, sys, argparse
from concurrent.futures import ThreadPoolExecutor, as_completed

import real_eval as R

CAP = 200        # memo makes moderate functions feasible; large ones are skipped
TIMEOUT = 40     # per program per config; partial capture keeps finished funcs
WORKERS = int(os.environ.get("EAN_WORKERS", "6"))
OUT = R.OUT

# (tag, ean, laws)
CONFIGS = [("aff_def", False, "safe"),
           ("aff_ean", True, "safe"),
           ("aff_kleene", True, "kleene")]


def process_program(suite, prog, bc):
    for tag, ean, laws in CONFIGS:
        R.invoke(bc, ["affine"], "default", ean, laws, CAP, 1, False,
                 R.raw_path(suite, prog, tag), profile=True, timeout=TIMEOUT,
                 monotone=True, memo=True)


def run(args):
    progs = R.programs(args.suite)
    if args.limit:
        progs = progs[:args.limit]
    workers = args.workers or WORKERS
    total = len(progs)
    sys.stderr.write(f"affine run: {total} programs x {len(CONFIGS)} configs, "
                     f"{workers} workers, cap={CAP}, timeout={TIMEOUT}s\n")
    done_n = 0
    with ThreadPoolExecutor(max_workers=workers) as ex:
        futs = {ex.submit(process_program, s, p, bc): (s, p)
                for (s, p, bc) in progs}
        for fut in as_completed(futs):
            s, p = futs[fut]
            done_n += 1
            try:
                fut.result()
            except Exception as e:  # noqa
                sys.stderr.write(f"  ERROR {s}/{p}: {e}\n")
            sys.stderr.write(f"[{done_n}/{total}] done {s}/{p}\n")
            sys.stderr.flush()
    sys.stderr.write("run complete\n")


def completed_funcs(res):
    """Functions whose affine analysis finished (have dagstats)."""
    out = {}
    for fn, cl in res["funcs"].items():
        a = cl.get("affine")
        if a and "nodes" in a:
            out[fn] = a
    return out


def agg(args):
    progs = R.programs()

    # ---- RQ1: affine kleene-soundness on real code -------------------------
    # Default vs EAN(safe) and Default vs EAN(kleene): both must be 0 unequal
    # (affine is distributive). Contrast with reachable/liveness where kleene
    # diverges (see real_rq1_kleene_divergence.csv).
    #
    # Every affine module times out (one slow function eventually hangs it), so
    # we cannot require complete files. Instead compare the COMPLETE PREFIX:
    # functions are emitted only after their analysis finishes, so all but the
    # LAST-emitted function of a truncated file are fully written. We drop that
    # last function from a timed-out side before comparing.
    def safe_funcs(path):
        d = R.in_by_func(path, "affine")
        if R.is_incomplete(path) and d:
            d.pop(next(reversed(d)))  # drop possibly-truncated last function
        return d

    with open(os.path.join(OUT, "affine_rq1.csv"), "w") as f:
        f.write("variant,laws,functions_compared,in_lines,unequal_lines,programs,crashes\n")
        for tag, laws in [("aff_ean", "safe"), ("aff_kleene", "kleene")]:
            total = unequal = nprog = ncrash = nfun = 0
            for suite, prog, bc in progs:
                dp = R.raw_path(suite, prog, "aff_def")
                ep = R.raw_path(suite, prog, tag)
                if not (os.path.exists(dp) and os.path.exists(ep)):
                    continue
                if R.is_crash(dp) or R.is_crash(ep):
                    ncrash += 1
                dd = safe_funcs(dp)
                ee = safe_funcs(ep)
                if not dd:
                    continue
                nprog += 1
                for fn, a in dd.items():
                    b = ee.get(fn)
                    if b is None or not a or not b:
                        continue  # not analyzed on both sides (cap skip / truncation)
                    nfun += 1
                    total += min(len(a), len(b))
                    for x, y in zip(a, b):
                        if x != y:
                            unequal += 1
                    unequal += abs(len(a) - len(b))
            f.write(f"{tag},{laws},{nfun},{total},{unequal},{nprog},{ncrash}\n")

    # ---- Table VII (affine) + memo break-even ------------------------------
    # Per function: unique nodes + memo interp + norm, Default vs EAN(safe).
    node_ratios, interp_ratios = [], []
    sum_norm = sum_interp_saved = 0
    n_pairs = 0
    with open(os.path.join(OUT, "affine_table7.csv"), "w") as f:
        f.write("metric,geomean_ean_over_default,n_functions\n")
        for suite, prog, bc in progs:
            dd = completed_funcs(R.parse(R.raw_path(suite, prog, "aff_def")))
            ee = completed_funcs(R.parse(R.raw_path(suite, prog, "aff_ean")))
            for fn, d in dd.items():
                e = ee.get(fn)
                if not e:
                    continue
                if d.get("nodes", 0) > 0 and e.get("nodes", 0) > 0:
                    node_ratios.append(e["nodes"] / d["nodes"])
                di, ei = d.get("interp", 0), e.get("interp", 0)
                if di > 0 and ei > 0:
                    interp_ratios.append(ei / di)
                    n_pairs += 1
                    sum_norm += e.get("norm", 0)
                    sum_interp_saved += max(0, di - ei)
        f.write(f"unique_nodes,{R.geomean(node_ratios):.4f},{len(node_ratios)}\n")
        f.write(f"memo_interp,{R.geomean(interp_ratios):.4f},{len(interp_ratios)}\n")

    with open(os.path.join(OUT, "affine_breakeven.csv"), "w") as f:
        f.write("n_functions,total_norm_us,total_interp_saved_us,breakeven_K\n")
        # K* = one-time EAN norm cost / per-run memo-interp saving. If EAN's
        # smaller IR makes each memo interpretation cheaper, EAN nets positive
        # after K* interpretations; if not (saving<=0), no break-even.
        k = (sum_norm / sum_interp_saved) if sum_interp_saved > 0 else float("inf")
        f.write(f"{n_pairs},{sum_norm},{sum_interp_saved},"
                f"{('%.1f' % k) if k != float('inf') else 'inf'}\n")

    # ---- Coverage (honest) -------------------------------------------------
    with open(os.path.join(OUT, "affine_coverage.csv"), "w") as f:
        f.write("suite,programs,completed_functions,timeouts,crashes\n")
        by_suite = {}
        for suite, prog, bc in progs:
            res = R.parse(R.raw_path(suite, prog, "aff_def"))
            s = by_suite.setdefault(suite, [0, 0, 0, 0])
            s[0] += 1
            s[1] += len(completed_funcs(res))
            s[2] += 1 if res["timeout"] else 0
            s[3] += 1 if res["crash"] else 0
        for suite, (np_, nf, nt, nc) in sorted(by_suite.items()):
            f.write(f"{suite},{np_},{nf},{nt},{nc}\n")

    sys.stderr.write("affine aggregate complete -> affine_*.csv in %s\n" % OUT)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--limit", type=int, default=0)
    r.add_argument("--suite", default=None)
    r.add_argument("--workers", type=int, default=0)
    sub.add_parser("agg")
    a = ap.parse_args()
    if a.cmd == "run":
        run(a)
    else:
        agg(a)

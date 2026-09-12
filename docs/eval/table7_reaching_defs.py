#!/usr/bin/env python3
"""reaching_defs Table VII (second client), coreutils, mirroring real_eval's
Table VII methodology exactly (per-function config/Default geomean for gen /
interp / end2end; norm vs Default generation; per-program peak RSS; timeouts).

Distinct raw tags `t7rd_*` so the reachable Table VII cache is untouched.
reaching_defs has richer facts than reachable -> bigger DAGs, so this is the
"expensive-client" second Table VII; large functions may time out (reported).

Usage:
  table7_reaching_defs.py run [--workers W]
  table7_reaching_defs.py agg
"""
import os, sys, argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import real_eval as R

CLIENT = "reaching_defs"
CAP = 300
REPEAT = 3        # ratios are relative; 3 repeats keep the geomean stable
TIMEOUT = 180
SUITE = "coreutils"

# (tag, ordering, ean, greedy, monotone)
CONFIGS = [
    ("t7rd_default", "default", False, False, False),
    ("t7rd_order", "cost-aware", False, False, False),
    ("t7rd_ean", "default", True, False, True),
    ("t7rd_order_ean", "cost-aware", True, False, True),
    ("t7rd_greedy", "default", False, True, False),
]
TAG = {"default": "t7rd_default", "order": "t7rd_order", "ean": "t7rd_ean",
       "order_ean": "t7rd_order_ean", "greedy": "t7rd_greedy"}


def process(suite, prog, bc):
    for tag, ordering, ean, greedy, mono in CONFIGS:
        R.invoke(bc, [CLIENT], ordering, ean, "safe", CAP, REPEAT, False,
                 R.raw_path(suite, prog, tag), timeout=TIMEOUT, monotone=mono,
                 greedy=greedy)
    return f"{suite}/{prog}"


def run(args):
    progs = R.programs(SUITE)
    workers = args.workers or R.WORKERS
    total = len(progs)
    sys.stderr.write(f"[t7-rd] {total} coreutils programs x {len(CONFIGS)} "
                     f"configs, {workers} workers\n")
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
    sys.stderr.write("[t7-rd] run complete\n")


def cfuncs(res):
    out = {}
    for fn, cl in res["funcs"].items():
        if CLIENT in cl and "nodes" in cl[CLIENT]:
            out[fn] = cl[CLIENT]
    return out


def agg(args):
    progs = R.programs(SUITE)
    variants = ["greedy", "order", "ean", "order_ean"]
    t7 = {c: {k: [] for k in ["gen", "norm", "interp", "end2end"]} for c in variants}
    rss = {c: [] for c in variants}
    timeouts = {c: 0 for c in ["default"] + variants}
    nfun = 0
    for suite, prog, bc in progs:
        parsed = {c: R.parse(R.raw_path(suite, prog, TAG[c]))
                  for c in TAG}
        for c in timeouts:
            if parsed[c]["timeout"]:
                timeouts[c] += 1
        base = cfuncs(parsed["default"])
        if not base:
            continue
        cf = {c: cfuncs(parsed[c]) for c in variants}
        for fn, b in base.items():
            if b.get("nodes", 0) <= 0:
                continue
            nfun += 1
            for c in variants:
                cd = cf[c].get(fn)
                if not cd or "nodes" not in cd:
                    continue
                for k in ["gen", "interp", "end2end"]:
                    bv, cv = b.get(k, 0), cd.get(k, 0)
                    if bv > 0 and cv > 0:
                        t7[c][k].append(cv / bv)
                if c in ("greedy", "ean", "order_ean") and b.get("gen", 0) > 0:
                    t7[c]["norm"].append(cd.get("norm", 0) / b["gen"])
        for c in variants:
            db, cb = parsed["default"]["mem"], parsed[c]["mem"]
            if db > 0 and cb > 0:
                rss[c].append(cb / db)

    def g(xs):
        v = R.geomean(xs)
        return v

    out = os.path.join(R.OUT, "real_table7_reaching_defs.csv")
    with open(out, "w") as f:
        f.write("configuration,generation,normalization,interpretation,"
                "end2end,peak_rss,timeouts,functions\n")
        for c, label in [("greedy", "Greedy"), ("order", "Order"),
                         ("ean", "EAN"), ("order_ean", "Order+EAN")]:
            norm = "—" if c == "order" else f"{g(t7[c]['norm']):.4f}"
            f.write(f"{label},{g(t7[c]['gen']):.4f},{norm},"
                    f"{g(t7[c]['interp']):.4f},{g(t7[c]['end2end']):.4f},"
                    f"{g(rss[c]):.4f},{timeouts[c]},{nfun}\n")
    sys.stderr.write(f"[t7-rd] wrote {out}\n")
    sys.stderr.write(f"  functions={nfun} timeouts={timeouts}\n")
    for c, label in [("greedy", "Greedy"), ("order", "Order"),
                     ("ean", "EAN"), ("order_ean", "Order+EAN")]:
        sys.stderr.write(f"  {label}: gen={g(t7[c]['gen']):.3f} "
                         f"interp={g(t7[c]['interp']):.3f} "
                         f"e2e={g(t7[c]['end2end']):.3f}\n")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--workers", type=int, default=0)
    sub.add_parser("agg")
    a = ap.parse_args()
    run(a) if a.cmd == "run" else agg(a)

#!/usr/bin/env python3
"""Real-bitcode evaluation driver for EAN (paper §IV, real corpus).

Runs lotus-dfa-apa over the bc14 corpus in the 4-config matrix
{Default, Order} x {no-EAN, EAN} plus a peak pass and a multi-client
correctness pass, then aggregates the parsed profile output into the paper's
Table IV / VI / VII and RQ1/RQ2/RQ3 CSVs.

Usage:
  real_eval.py run   [--limit N] [--suite coreutils|open|spec]
  real_eval.py agg

Raw tool outputs are cached under raw/ so `run` is resumable and `agg` is
re-runnable without re-executing the tool.
"""
import os, sys, re, glob, subprocess, statistics, math, argparse, json
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.environ.get("LOTUS_DFA_APA",
                      "D:/Code/lotus/lotus/build/bin/lotus-dfa-apa.exe")
BC_ROOT = os.environ.get("BC14", "D:/Code/lotus/bc14/bc14")
RAW = os.path.join(HERE, "raw")
OUT = HERE  # CSVs written next to this script (docs/eval)

CAP = 300            # --max-func-insts for structural/timing (deterministic, fast)
STRUCT_TIMEOUT = 180
PEAK_CAP = 150       # tighter cap for the expensive peak pass
PEAK_TIMEOUT = 180
CC_CAP = 150         # correctness pass: smaller cap (slow clients time out large fns)
CC_TIMEOUT = 45      # correctness pass: short timeout (a slow fn is just excluded)
REPEAT = 5           # measured runs per function (timing median)
INTERP_REPEAT = 50   # per-query interpretation repeats (amortization, RQ2)
WORKERS = int(os.environ.get("EAN_WORKERS", "6"))
EAN_BUDGET = ["--ean-round-limit=30", "--ean-node-limit=200000",
              "--ean-time-limit=5"]

DISTRIB = ["reachable", "reaching_defs", "liveness"]
# Fully distributive clients: sound under the full Kleene profile (verified
# 0 unequal facts). available_exprs is excluded from correctness (facts embed
# raw expression pointers, non-deterministic across processes). uninitialized +
# constant_prop are NOT fully distributive (right-distributivity / sliding
# change their results), so they use the universally safe-minimal profile.
NONDISTRIB = ["uninitialized", "constant_prop"]

SUITES = ["coreutils", "open", "spec"]

# ----------------------------------------------------------------- running ---
def programs(suite=None):
    out = []
    for s in (SUITES if suite is None else [suite]):
        for bc in sorted(glob.glob(os.path.join(BC_ROOT, s, "*.bc"))):
            if "__MACOSX" in bc:
                continue
            out.append((s, os.path.basename(bc)[:-3], bc))
    return out

def raw_path(suite, prog, tag):
    d = os.path.join(RAW, suite)
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, f"{prog}.{tag}.txt")

def done(path):
    if not os.path.exists(path):
        return False
    try:
        with open(path, "rb") as f:
            f.seek(max(0, os.path.getsize(path) - 4096))
            tail = f.read().decode("utf-8", "ignore")
        return ("[mem] peak_rss_kb=" in tail or "[TIMEOUT]" in tail
                or "[CRASH" in tail)
    except OSError:
        return False

def invoke(bc, clients, ordering, ean, laws, cap, repeat, peak, outpath,
           profile=True, timeout=STRUCT_TIMEOUT, interp_repeat=1, monotone=False,
           greedy=False, memo=False):
    if done(outpath):
        return "cached"
    cmd = [TOOL, bc, "--stdout", "--elim-method=state",
           f"--analysis={','.join(clients)}", f"--ordering={ordering}",
           f"--max-func-insts={cap}", f"--repeat={repeat}",
           f"--interp-repeat={interp_repeat}"]
    if profile:
        cmd.append("--dump-profile")
    if peak:
        cmd.append("--measure-peak")
    if greedy:
        cmd.append("--greedy")
    if memo:
        cmd.append("--memo-interp")
    if ean:
        cmd += ["--ean", f"--ean-laws={laws}"] + EAN_BUDGET
        if monotone:
            cmd.append("--ean-monotone")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        txt = r.stdout
        if "[mem] peak_rss_kb=" not in txt:
            txt += f"\n[CRASH rc={r.returncode}]\n"
    except subprocess.TimeoutExpired as e:
        # Keep whatever the tool streamed before the kill: a huge module still
        # contributes the functions it finished (partial data, marked TIMEOUT).
        partial = e.stdout or e.output or ""
        if isinstance(partial, bytes):
            partial = partial.decode("utf-8", "ignore")
        txt = partial + f"\n[TIMEOUT] {timeout}s\n"
    with open(outpath, "w", encoding="utf-8") as f:
        f.write(txt)
    return "ran"

def process_program(suite, prog, bc):
    # Pass 1: structural + timing, reachable, 4 configs. EAN uses the SOUND
    # safe-minimal profile (left-distributivity only) — it preserves every
    # client's results (RQ1) and still yields the bulk of the node reduction.
    # EAN configs use the monotone guard so EAN never degrades its input.
    for tag, ordering, ean, mono in [("default", "default", False, False),
                                     ("order", "cost-aware", False, False),
                                     ("ean", "default", True, True),
                                     ("order_ean", "cost-aware", True, True)]:
        invoke(bc, ["reachable"], ordering, ean, "safe", CAP, REPEAT, False,
               raw_path(suite, prog, tag), timeout=STRUCT_TIMEOUT, monotone=mono)
    # Greedy config (deterministic simplifier, no reuse-aware extraction).
    invoke(bc, ["reachable"], "default", False, "safe", CAP, REPEAT, False,
           raw_path(suite, prog, "greedy"), timeout=STRUCT_TIMEOUT, greedy=True)
    # Pass 2: peak, reachable, default + cost-aware.
    for tag, ordering in [("peak_default", "default"), ("peak_order", "cost-aware")]:
        invoke(bc, ["reachable"], ordering, False, "safe", PEAK_CAP, 1, True,
               raw_path(suite, prog, tag), timeout=PEAK_TIMEOUT)
    # Pass 2b: amortization — interpret each summary INTERP_REPEAT times so the
    # per-query interpretation cost is measurable (RQ2 break-even), Default vs EAN.
    invoke(bc, ["reachable"], "default", False, "safe", CAP, 1, False,
           raw_path(suite, prog, "amort_default"), timeout=STRUCT_TIMEOUT,
           interp_repeat=INTERP_REPEAT)
    invoke(bc, ["reachable"], "default", True, "safe", CAP, 1, False,
           raw_path(suite, prog, "amort_ean"), timeout=STRUCT_TIMEOUT,
           interp_repeat=INTERP_REPEAT)
    # Pass 3: correctness (default vs EAN), PER CLIENT (crash-isolated). All
    # clients use safe-minimal (the sound universal profile) -> expect 0 unequal.
    for cl in DISTRIB + NONDISTRIB:
        invoke(bc, [cl], "default", False, "safe", CC_CAP, 1, False,
               raw_path(suite, prog, f"cc_{cl}_def"), profile=False, timeout=CC_TIMEOUT)
        invoke(bc, [cl], "default", True, "safe", CC_CAP, 1, False,
               raw_path(suite, prog, f"cc_{cl}_ean"), profile=False, timeout=CC_TIMEOUT)
        invoke(bc, [cl], "default", False, "safe", CC_CAP, 1, False,
               raw_path(suite, prog, f"cc_{cl}_greedy"), profile=False,
               timeout=CC_TIMEOUT, greedy=True)
    return f"{suite}/{prog}"

def run(args):
    progs = programs(args.suite)
    if args.limit:
        progs = progs[:args.limit]
    total = len(progs)
    workers = args.workers or WORKERS
    sys.stderr.write(f"running {total} programs with {workers} workers\n")
    sys.stderr.flush()
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

# --------------------------------------------------------------- parsing -----
RE = {
    "func": re.compile(r"^FUNC (\S+)"),
    "skip": re.compile(r"\[skipped\] reason=too_large insts=(\d+)"),
    "client": re.compile(r"\[client:(\w+)\]"),
    "cfg": re.compile(r"\[cfg\] args=(\d+), blocks=(\d+), insts=(\d+), edges=(\d+),.*elapsed_us=(\d+)"),
    "timing": re.compile(r"\[timing\] gen_us=(\d+), norm_us=(\d+), interp_us=(\d+), end2end_us=(\d+), runs=(\d+)"),
    "solver": re.compile(r"\[solver\].*peak_nodes=(\d+)"),
    "dag": re.compile(r"\[dagstats\] nodes=(\d+), edges=(\d+), tree=([\d.e+]+), seq=(\d+), stars=(\d+), unions=(\d+), atoms=(\d+), sharing=([\d.e+]+), roots=(\d+)"),
    "mem": re.compile(r"\[mem\] peak_rss_kb=(\d+)"),
    "skipsum": re.compile(r"\[summary\] skipped_functions=(\d+)"),
}

def parse(path):
    """Return dict: {'timeout':bool, 'mem':int, 'skipped':int,
       'funcs': {fname: {client: {cfg..., timing..., dag..., peak}}}}"""
    res = {"timeout": False, "crash": False, "mem": 0, "skipped": 0, "funcs": {}}
    if not os.path.exists(path):
        return res
    cur_f = None
    cur_c = None
    with open(path, encoding="utf-8") as f:
        for ln in f:
            if "[TIMEOUT]" in ln:
                res["timeout"] = True
            if "[CRASH" in ln:
                res["crash"] = True
            m = RE["func"].search(ln)
            if m:
                cur_f = m.group(1); cur_c = None
                res["funcs"].setdefault(cur_f, {})
                continue
            m = RE["client"].search(ln)
            if m:
                cur_c = m.group(1)
                if cur_f is not None:
                    res["funcs"][cur_f].setdefault(cur_c, {})
                continue
            m = RE["cfg"].search(ln)
            if m and cur_f is not None and cur_c is not None:
                d = res["funcs"][cur_f][cur_c]
                d["args"], d["blocks"], d["insts"], d["edges"], d["elapsed"] = map(int, m.groups())
                continue
            m = RE["timing"].search(ln)
            if m and cur_f is not None and cur_c is not None:
                d = res["funcs"][cur_f][cur_c]
                d["gen"], d["norm"], d["interp"], d["end2end"], d["runs"] = map(int, m.groups())
                continue
            m = RE["solver"].search(ln)
            if m and cur_f is not None and cur_c is not None:
                res["funcs"][cur_f][cur_c]["peak"] = int(m.group(1))
                continue
            m = RE["dag"].search(ln)
            if m and cur_f is not None and cur_c is not None:
                d = res["funcs"][cur_f][cur_c]
                d["nodes"], d["dedges"], d["tree"], d["seq"], d["stars"], d["unions"], d["atoms"], d["sharing"], d["roots"] = (
                    int(m.group(1)), int(m.group(2)), float(m.group(3)), int(m.group(4)),
                    int(m.group(5)), int(m.group(6)), int(m.group(7)), float(m.group(8)), int(m.group(9)))
                continue
            m = RE["skip"].search(ln)
            if m:
                res["skipped"] += 1
                continue
            m = RE["mem"].search(ln)
            if m:
                res["mem"] = int(m.group(1))
    return res

# ------------------------------------------------------------- aggregation ---
def geomean(ratios):
    rs = [r for r in ratios if r is not None and r > 0]
    if not rs:
        return float("nan")
    return math.exp(sum(math.log(r) for r in rs) / len(rs))

def collect_reachable(res):
    """function -> reachable metric dict (structural + timing)."""
    out = {}
    for fn, cl in res["funcs"].items():
        if "reachable" in cl and "nodes" in cl["reachable"]:
            out[fn] = cl["reachable"]
    return out

def agg(args):
    progs = programs()
    # ---- Table IV corpus + Table VI/VII + RQ2 (from reachable, 4 configs) ----
    fam = {}  # suite -> aggregate
    t6 = {c: {k: [] for k in ["nodes", "dedges", "tree", "seq", "stars"]}
          for c in ["greedy", "order", "ean", "order_ean"]}
    t6_share = {c: [] for c in ["default", "greedy", "order", "ean", "order_ean"]}
    t7 = {c: {k: [] for k in ["gen", "norm", "interp", "end2end"]}
          for c in ["greedy", "order", "ean", "order_ean"]}
    t7_rss = {c: [] for c in ["greedy", "order", "ean", "order_ean"]}
    timeouts = {c: 0 for c in ["default", "greedy", "order", "ean", "order_ean"]}
    breakeven = []  # (default_nodes, default_end2end_us, ean_end2end_us)

    for suite, prog, bc in progs:
        parsed = {c: parse(raw_path(suite, prog, c))
                  for c in ["default", "greedy", "order", "ean", "order_ean"]}
        for c in timeouts:
            if parsed[c]["timeout"]:
                timeouts[c] += 1
        rc = {c: collect_reachable(parsed[c]) for c in parsed}
        # corpus (Table IV) from default; only count programs that produced data
        fa = fam.setdefault(suite, dict(programs=0, functions=0, insts=0,
                                        edges=0, roots=0, nodes=0, stars=0,
                                        skipped=0, timeouts=0))
        if parsed["default"]["timeout"] or parsed["default"]["crash"]:
            fa["timeouts"] += 1
        if not rc["default"]:
            continue  # excluded program (crash/timeout/empty) — no corpus rows
        fa["programs"] += 1
        fa["skipped"] += parsed["default"]["skipped"]
        for fn, d in rc["default"].items():
            fa["functions"] += 1
            fa["insts"] += d.get("insts", 0)
            fa["edges"] += d.get("edges", 0)
            fa["roots"] += d.get("roots", 0)
            fa["nodes"] += d.get("nodes", 0)
            fa["stars"] += d.get("stars", 0)
        # per-function ratios vs default
        for fn, dd in rc["default"].items():
            base = dd
            if base.get("nodes", 0) <= 0:
                continue
            t6_share["default"].append(base.get("sharing", 0.0))
            for c in ["greedy", "order", "ean", "order_ean"]:
                cd = rc[c].get(fn)
                if not cd or "nodes" not in cd:
                    continue
                for k in ["nodes", "dedges", "tree", "seq", "stars"]:
                    b = base.get(k, 0); v = cd.get(k, 0)
                    if b > 0 and v > 0:
                        t6[c][k].append(v / b)
                t6_share[c].append(cd.get("sharing", 0.0))
                for k in ["gen", "interp", "end2end"]:
                    b = base.get(k, 0); v = cd.get(k, 0)
                    if b > 0 and v > 0:
                        t7[c][k].append(v / b)
                # normalization ratio is vs default generation (EAN adds it)
                if c in ("greedy", "ean", "order_ean") and base.get("gen", 0) > 0:
                    t7[c]["norm"].append(cd.get("norm", 0) / base["gen"])
            # breakeven: default vs ean end2end by raw dag size
            ed = rc["ean"].get(fn)
            if ed and base.get("end2end", 0) > 0 and ed.get("end2end", 0) > 0:
                breakeven.append((base["nodes"], base["end2end"], ed["end2end"]))
        # peak RSS per program (config vs default)
        for c in ["greedy", "order", "ean", "order_ean"]:
            db, cb = parsed["default"]["mem"], parsed[c]["mem"]
            if db > 0 and cb > 0:
                t7_rss[c].append(cb / db)

    # ---- write Table IV ----
    with open(os.path.join(OUT, "real_table4_corpus.csv"), "w") as f:
        f.write("family,programs,functions,insts,edges,roots,dag_nodes,star_nodes,skipped_functions,timeouts\n")
        tot = dict(programs=0, functions=0, insts=0, edges=0, roots=0, nodes=0, stars=0, skipped=0, timeouts=0)
        for s in SUITES:
            if s not in fam:
                continue
            a = fam[s]
            f.write(f"{s},{a['programs']},{a['functions']},{a['insts']},{a['edges']},{a['roots']},{a['nodes']},{a['stars']},{a['skipped']},{a['timeouts']}\n")
            for k in tot:
                tot[k] += a[k]
        f.write(f"Total,{tot['programs']},{tot['functions']},{tot['insts']},{tot['edges']},{tot['roots']},{tot['nodes']},{tot['stars']},{tot['skipped']},{tot['timeouts']}\n")

    # ---- write Table VI ----
    with open(os.path.join(OUT, "real_table6_complexity.csv"), "w") as f:
        f.write("configuration,unique_nodes,dag_edges,tree_size,sequence,stars,sharing_before,sharing_after\n")
        sb = statistics.mean(t6_share["default"]) if t6_share["default"] else 0.0
        for c, name in [("greedy", "Greedy"), ("order", "Order"), ("ean", "EAN"), ("order_ean", "Order+EAN")]:
            sa = statistics.mean(t6_share[c]) if t6_share[c] else 0.0
            f.write(f"{name},{geomean(t6[c]['nodes']):.4f},{geomean(t6[c]['dedges']):.4f},"
                    f"{geomean(t6[c]['tree']):.4f},{geomean(t6[c]['seq']):.4f},"
                    f"{geomean(t6[c]['stars']):.4f},{sb:.3f},{sa:.3f}\n")

    # ---- write Table VII ----
    with open(os.path.join(OUT, "real_table7_performance.csv"), "w") as f:
        f.write("configuration,generation,normalization,interpretation,end2end,peak_rss,timeouts\n")
        for c, name in [("greedy", "Greedy"), ("order", "Order"), ("ean", "EAN"), ("order_ean", "Order+EAN")]:
            norm = geomean(t7[c]["norm"]) if t7[c]["norm"] else float("nan")
            f.write(f"{name},{geomean(t7[c]['gen']):.4f},{norm:.4f},"
                    f"{geomean(t7[c]['interp']):.4f},{geomean(t7[c]['end2end']):.4f},"
                    f"{geomean(t7_rss[c]):.4f},{timeouts[c]}\n")

    # ---- RQ2 breakeven ----
    with open(os.path.join(OUT, "real_rq2_breakeven.csv"), "w") as f:
        f.write("bucket_raw_nodes,n,default_end2end_us_median,ean_end2end_us_median,ean_over_default\n")
        buckets = [(0, 50), (50, 100), (100, 200), (200, 500), (500, 1000),
                   (1000, 5000), (5000, 10**9)]
        for lo, hi in buckets:
            sel = [(d, e) for (n, d, e) in breakeven if lo <= n < hi]
            if not sel:
                continue
            dmed = statistics.median([d for d, _ in sel])
            emed = statistics.median([e for _, e in sel])
            f.write(f"{lo}-{hi},{len(sel)},{dmed:.0f},{emed:.0f},{(emed/dmed if dmed else 0):.3f}\n")

    # ---- RQ3 peak (peak pass) ----
    with open(os.path.join(OUT, "real_rq3_peak.csv"), "w") as f:
        f.write("family,functions,default_peak_mean,order_peak_mean,peak_ratio_geomean\n")
        for s in SUITES:
            ratios = []
            dsum = osum = 0
            nfun = 0
            for suite, prog, bc in [p for p in progs if p[0] == s]:
                pd = collect_reachable(parse(raw_path(s, prog, "peak_default")))
                po = collect_reachable(parse(raw_path(s, prog, "peak_order")))
                for fn, d in pd.items():
                    o = po.get(fn)
                    if o and d.get("peak", 0) > 0 and o.get("peak", 0) > 0:
                        ratios.append(o["peak"] / d["peak"])
                        dsum += d["peak"]; osum += o["peak"]; nfun += 1
            if nfun:
                f.write(f"{s},{nfun},{dsum/nfun:.1f},{osum/nfun:.1f},{geomean(ratios):.4f}\n")

    # ---- RQ1 correctness (default vs EAN and vs Greedy, per client) ----
    with open(os.path.join(OUT, "real_rq1_correctness.csv"), "w") as f:
        f.write("variant,client,laws,functions_compared,in_lines,unequal_lines,programs,crashes\n")
        for variant in ["ean", "greedy"]:
            for client, laws in [(c, "safe") for c in DISTRIB + NONDISTRIB]:
                total = unequal = nprog = ncrash = nfun = 0
                for suite, prog, bc in progs:
                    dp = raw_path(suite, prog, f"cc_{client}_def")
                    ep = raw_path(suite, prog, f"cc_{client}_{variant}")
                    if not (os.path.exists(dp) and os.path.exists(ep)):
                        continue
                    if is_crash(dp) or is_crash(ep):
                        ncrash += 1
                    # Only compare COMPLETE pairs: a truncated (timed-out) dump
                    # cuts the last function mid-states -> fabricated mismatches.
                    if is_incomplete(dp) or is_incomplete(ep):
                        continue
                    dd = in_by_func(dp, client)
                    ee = in_by_func(ep, client)
                    if not dd:
                        continue
                    nprog += 1
                    for fn, a in dd.items():
                        b = ee.get(fn)
                        if b is None:
                            continue
                        # Skip functions not analyzed on BOTH sides (cap skip):
                        # an empty side must not fabricate diffs.
                        if not a or not b:
                            continue
                        nfun += 1
                        total += min(len(a), len(b))
                        for x, y in zip(a, b):
                            if x != y:
                                unequal += 1
                        unequal += abs(len(a) - len(b))
                f.write(f"{variant},{client},{laws},{nfun},{total},{unequal},{nprog},{ncrash}\n")
    agg_rq2_extra(progs)
    sys.stderr.write("aggregate complete -> CSVs in %s\n" % OUT)

def agg_rq2_extra(progs):
    """RQ2: invocation-gate sweep + amortized break-even (memo vs non-memo)."""
    # ---- Gate sweep (analytical over per-function default vs EAN end-to-end) --
    # gated end-to-end: functions below the raw-node threshold keep Default
    # (EAN skipped); the rest pay EAN. No extra runs needed.
    fn_rows = []  # (raw_nodes_default, e2e_default, e2e_ean)
    for suite, prog, bc in progs:
        dd = collect_reachable(parse(raw_path(suite, prog, "default")))
        ee = collect_reachable(parse(raw_path(suite, prog, "ean")))
        for fn, d in dd.items():
            e = ee.get(fn)
            if not e or "end2end" not in d or "end2end" not in e:
                continue
            fn_rows.append((d.get("nodes", 0), d["end2end"], e["end2end"]))
    base_total = sum(d for _, d, _ in fn_rows) or 1
    with open(os.path.join(OUT, "real_rq2_gate.csv"), "w") as f:
        f.write("gate_min_nodes,functions_total,functions_ean,gated_end2end_ratio,ungated_ean_ratio\n")
        ean_total = sum(e for _, _, e in fn_rows)
        for T in [0, 25, 50, 100, 200, 500, 1000, 2000, 5000]:
            gated = sum((e if n >= T else d) for n, d, e in fn_rows)
            n_ean = sum(1 for n, _, _ in fn_rows if n >= T)
            f.write(f"{T},{len(fn_rows)},{n_ean},{gated/base_total:.4f},"
                    f"{ean_total/base_total:.4f}\n")

    # ---- Amortized break-even (from the interp-repeat=INTERP_REPEAT passes) ---
    S = dict(gen_d=0.0, i1_d=0.0, gen_e=0.0, norm_e=0.0, i1_e=0.0,
             im_d=0.0, im_e=0.0, n=0)
    for suite, prog, bc in progs:
        ad = collect_reachable(parse(raw_path(suite, prog, "amort_default")))
        ae = collect_reachable(parse(raw_path(suite, prog, "amort_ean")))
        for fn, d in ad.items():
            e = ae.get(fn)
            if not e:
                continue
            treed, nodesd = d.get("tree", 0.0), d.get("nodes", 0)
            treee, nodese = e.get("tree", 0.0), e.get("nodes", 0)
            if "interp" not in d or "interp" not in e or treed <= 0 or treee <= 0:
                continue
            i1d = d["interp"] / INTERP_REPEAT      # per-query, non-memo (∝ tree)
            i1e = e["interp"] / INTERP_REPEAT
            S["gen_d"] += d.get("gen", 0)
            S["gen_e"] += e.get("gen", 0)
            S["norm_e"] += e.get("norm", 0)
            S["i1_d"] += i1d
            S["i1_e"] += i1e
            # Memoizing projection: a memo interpreter evaluates each UNIQUE node
            # once, so per-query cost scales by unique/tree (= 1/sharing).
            S["im_d"] += i1d * (nodesd / treed)
            S["im_e"] += i1e * (nodese / treee)
            S["n"] += 1

    def breakeven(fixed_e, fixed_d, per_e, per_d):
        # smallest K>=1 with fixed_e + K*per_e < fixed_d + K*per_d
        if per_e >= per_d:
            return None  # EAN never cheaper per query -> no break-even
        import math as _m
        k = (fixed_e - fixed_d) / (per_d - per_e)
        return max(1, int(_m.ceil(k)))

    with open(os.path.join(OUT, "real_rq2_amortized.csv"), "w") as f:
        f.write("model,gen_def_us,gen_ean_us,norm_ean_us,interp_per_query_def_us,"
                "interp_per_query_ean_us,per_query_speedup,breakeven_K\n")
        # non-memoizing (measured): cost ∝ expanded tree
        spd = (S["i1_d"] / S["i1_e"]) if S["i1_e"] else float("nan")
        k = breakeven(S["gen_e"] + S["norm_e"], S["gen_d"], S["i1_e"], S["i1_d"])
        f.write(f"non_memoizing,{S['gen_d']:.0f},{S['gen_e']:.0f},{S['norm_e']:.0f},"
                f"{S['i1_d']:.1f},{S['i1_e']:.1f},{spd:.3f},{k if k else 'none'}\n")
        # memoizing (projected): cost ∝ unique nodes
        spdm = (S["im_d"] / S["im_e"]) if S["im_e"] else float("nan")
        km = breakeven(S["gen_e"] + S["norm_e"], S["gen_d"], S["im_e"], S["im_d"])
        f.write(f"memoizing_projected,{S['gen_d']:.0f},{S['gen_e']:.0f},{S['norm_e']:.0f},"
                f"{S['im_d']:.1f},{S['im_e']:.1f},{spdm:.3f},{km if km else 'none'}\n")

def is_crash(path):
    try:
        with open(path, "rb") as f:
            f.seek(max(0, os.path.getsize(path) - 512))
            return "[CRASH" in f.read().decode("utf-8", "ignore")
    except OSError:
        return False

def is_incomplete(path):
    """True if the run timed out or crashed (truncated output)."""
    try:
        with open(path, "rb") as f:
            f.seek(max(0, os.path.getsize(path) - 512))
            tail = f.read().decode("utf-8", "ignore")
        return ("[TIMEOUT]" in tail or "[CRASH" in tail
                or "[mem] peak_rss_kb=" not in tail)
    except OSError:
        return True

def in_by_func(path, client):
    """{func_name: [ordered IN state lines]} for the given client section."""
    out = {}
    cur_f = None
    cur_c = None
    with open(path, encoding="utf-8") as f:
        for ln in f:
            m = RE["func"].search(ln)
            if m:
                cur_f = m.group(1); cur_c = None
                out.setdefault(cur_f, [])
                continue
            m = RE["client"].search(ln)
            if m:
                cur_c = m.group(1)
                continue
            if cur_f is not None and cur_c == client and " IN" in ln and ":" in ln:
                out[cur_f].append(ln.strip())
    return out

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run"); r.add_argument("--limit", type=int, default=0)
    r.add_argument("--suite", default=None)
    r.add_argument("--workers", type=int, default=0)
    sub.add_parser("agg")
    a = ap.parse_args()
    if a.cmd == "run":
        run(a)
    else:
        agg(a)

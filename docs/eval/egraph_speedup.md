# E-graph optimization speedup (A + B-safe, byte-identical)

Measures the effect of the work-queue extractors (core `Extract.h::Extractor`
and EAN `BatchExtract.h::cycleSafeExtract`) plus the B-safe rebuild guard
(`EGraph::rebuild` skips `recomputeParents()` when a pass performed no unions).
Output is byte-identical (proven: `EanBatchExtract.WorklistExtractMatchesNaive`
+ real-corpus dagstats unchanged), so this is a pure engineering speedup.

Metric: `[timing] norm_us` = end-to-end EAN cost (import + saturation with
per-round extraction + final reuse-aware extraction), summed per function.
Config: `--analysis=reaching_defs --ean --ean-laws=safe --max-func-insts=200
--repeat=3`, default (Tree) plateau mode. Corpus: coreutils subset
{dirname, basename, cksum, comm, b2sum, base64, cat, chcon}, 1199 functions.

| build | total norm_us | max/func norm_us | functions |
|---|---|---|---|
| baseline (HEAD, fixpoint rescan) | 10,576,395 | 278,673 | 1199 |
| optimized (A + B-safe) | 8,020,467 | 283,058 | 1199 |

**Speedup: 1.32× (−24% total EAN normalization time).** The win is in
aggregate; the single largest function is unchanged (its cost is dominated by
non-extraction work — factorization/import), consistent with extraction being
one significant component of `norm_us` that was roughly halved.

Note: default Tree plateau mode calls the core extractor once per saturation
round; Dag plateau mode (reuse-aware extraction every round) would show a larger
gap. Timing has modest run-to-run variance; the 24% aggregate delta is well
above noise.

## After step 2 (typed DSL + B-full)

typed PathLang (numeric atom ordering, no per-node string alloc/hash) + full
incremental parents (recomputeParents now canonicalize+dedup existing lists,
O(sum parents), no whole-graph node rescan). Same corpus subset / config.

| build | total norm_us | max/func norm_us | vs HEAD |
|---|---|---|---|
| baseline (HEAD) | 10,576,395 | 278,673 | 1.00× |
| A + B-safe | 8,020,467 | 283,058 | 1.32× |
| **A + typed DSL + B-full** | **6,155,253** | **136,012** | **1.72× (−41.8%)** |

The largest single function drops ~2× (278,673 → 136,012 µs): typed nodes remove
per-node string hashing and B-full removes the O(#nodes) parent rescan, both of
which hit large deep DAGs hardest. Output: facts identical vs Default on real bc
(dirname reaching_defs, all 2984 lines); dagstats node counts unchanged on
dirname (654/109) — extraction cost is invariant to tie-break, so Table VI is
expected to move little. Full corpus refresh + golden update is step 4.

## Paper footnote (implementation efficiency)

Draft text for the paper's implementation/RQ2 discussion (no LaTeX source in
repo; paste-ready English):

> **Implementation note.** EAN's e-graph represents path expressions with a
> *typed* node language — one tagged variant per operator (zero, one, atom, seq,
> join, star) with an integer-keyed atom payload — rather than string operators;
> it maintains e-class parent lists incrementally across rebuilds instead of
> rescanning all nodes, and extracts the shared DAG with a parent-pointer work
> queue rather than a whole-graph relaxation-to-fixpoint. On extraction-heavy
> clients (e.g. reaching definitions) these engine choices lower EAN's
> normalization time by ≈1.7× relative to a naive implementation. Because they
> change only the engine and not the algebra, the extracted DAGs — and therefore
> all client results — are unchanged: we re-verified zero divergent facts across
> the entire corpus (1.65M/0.71M/1.06M/1.65M/0.16M IN lines for reachable /
> reaching-defs / liveness / uninitialized / constant-prop, all 0 unequal).

Supporting data: this file (1.72× on the coreutils reaching_defs subset),
`real_rq1_correctness.csv` (0 unequal, all clients/configs), `affine_rq1.csv`
(0 unequal, safe + kleene).

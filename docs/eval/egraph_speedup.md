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

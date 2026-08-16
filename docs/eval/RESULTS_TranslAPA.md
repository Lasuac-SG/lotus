# TranslAPA Baseline — Three-Way Comparison (B4)

Comparison of three interpreters of the **same** elimination front-end
path-expression DAG (`Results.ExprTo(N)`), on the `bc14` corpus:

- **Default** — framework generic interpreter (`SolverContext::eval`: transfer
  re-application, `Star` iterated to a lattice fixpoint).
- **EAN** — our equality-saturation DAG optimizer, then generic eval.
- **TranslAPA** — the paper's closed-form Gen/Kill semiring fold (OOPSLA 2026;
  `Star` is O(1), single memoized bottom-up pass). External strong baseline.

Metric split (paper's construction-vs-evaluation): the one-time IDA→APA
translation cost (TranslAPA mechanical extraction / EAN saturation) is recorded
in `norm_us`; the per-query evaluation cost in `interp_us`. All three share
`gen_us` (front-end) and the same raw DAG, so `nodes` differ only for EAN.

## Reproducing

```
# harness (resumable; caches raw tool output under docs/eval/raw/)
python3 docs/eval/translapa_eval.py run --suite coreutils [--limit N] [--workers W]
python3 docs/eval/translapa_eval.py agg --suite coreutils    # -> translapa_compare.csv
```

`--interp={generic|translapa}` on `lotus-dfa-apa` selects the interpreter;
`--ean` selects the optimizer. `--max-func-insts` caps per-function cost
(state elimination is O(n³) per function; the corpus is statically-linked so a
full run is a multi-hour batch — the cache makes it resumable).

## Results (sample: 4 modules — libstdbuf.so, make-prime-list, dirname, whoami; cap=200, repeat=1)

Ratios are geomean of per-function `config / Default`; lower is better.

### reaching_defs (3,220 functions, separable Gen/Kill)

| Metric | Default | EAN | TranslAPA |
|---|---|---|---|
| DAG unique nodes (ratio) | 1.00 | **0.27** | 1.00 |
| Per-query eval `interp` (ratio) | 1.00 | 1.05 | **0.69** |
| One-time transform (µs, median) | — | 838 (saturation) | **104 (extraction)** |
| End-to-end (ratio) | 1.00 | 3.60 | 1.67 |
| Facts vs Default (unequal / compared) | — | — | **0 / 99026** |

### reachable (2,663 functions, degenerate 1-fact lattice)

| Metric | Default | EAN | TranslAPA |
|---|---|---|---|
| DAG unique nodes (ratio) | 1.00 | 0.27 | 1.00 |
| Per-query eval `interp` (ratio) | 1.00 | 1.17 | 8.98 |
| Facts vs Default (unequal / compared) | — | — | **0 / 99026** |

## Reading of the results

1. **Correctness (headline).** TranslAPA reproduces the framework's generic
   result **bit-for-bit**: 0 unequal IN lines out of 99,026 for both clients.
   This is the paper's "computes the same set of dataflow facts" guarantee,
   verified empirically against an independent interpreter.

2. **DAG node count.** TranslAPA keeps the raw DAG (ratio 1.00) — it changes the
   *semantic function*, not the graph. EAN cuts the DAG to **~27%** of nodes.
   These are orthogonal, composable levers: EAN shrinks the graph; TranslAPA
   folds it in closed form. (A natural next experiment: TranslAPA *on the
   EAN-reduced DAG*.)

3. **Evaluation time.** On reaching_defs, the closed-form fold is **0.69×** the
   generic eval per query — a real per-query speedup from replacing Star
   iteration with the O(1) closed form. EAN's generic-eval-on-smaller-DAG is
   ~1.05× here: on these mostly loop-free / small functions the tree-walk is
   already cheap, so node reduction does not translate into a single-shot eval
   win (EAN's eval payoff shows under amortized/memoized interpretation — see
   `real_rq2_amortized.csv`).

4. **One-time cost.** TranslAPA's mechanical extraction (~104 µs median) is
   ~8× cheaper than EAN's saturation (~838 µs median), so TranslAPA's
   end-to-end overhead (1.67×) is well below EAN's (3.60×) on this sample.

5. **`reachable` is degenerate.** With a single boolean fact, generic eval is a
   trivial OR (~9 µs) while TranslAPA still folds the whole DAG into 1-bit
   Gen/Kill elements (~92 µs), so the closed form loses here. Expected: the
   powerset framing has no leverage on a one-element lattice. reaching_defs (and,
   for B3, the IFDS clients) are the meaningful demonstrators.

## Caveats

- Sample of 4 modules at cap=200, repeat=1 — indicative, not the full corpus.
  Run `translapa_eval.py run` (no `--limit`) for the whole suite; it is
  resumable via the raw cache.
- `interp_us` is a single median run (repeat=1); use `--repeat` ≥ 3 for stable
  timing on the full run.
- EAN correctness across the corpus is validated separately by
  `real_eval.py` RQ1 (`real_rq1_correctness.csv`); here we only re-verified
  Default-vs-TranslAPA.

## EAN ⊕ TranslAPA — composing two orthogonal levers (full coreutils, 4-way)

The two techniques act on different parts of the same pipeline: **EAN shrinks
the path-expression DAG** (fewer unique nodes, semantics preserved under
safe-minimal laws) while **TranslAPA changes the semantic function** (closed-form
Gen/Kill fold, `Star` in O(1) instead of a lattice fixpoint). They are
composable — fold the EAN-reduced DAG — which the tool now supports directly via
`--ean --interp=translapa` (the client sets `InterpMemo` so the EAN post-pass
rewrites `ExprTo` but skips its discarded generic eval; construction time then
accounts EAN saturation **plus** mechanical extraction).

This run is the **whole coreutils suite (109 programs, 4 configs)**, not the
4-module sample above; `translapa_compare.csv` and `translapa_compose.csv` were
regenerated over all 109. Ratios are geomean of per-function `config / Default`
(lower is better).

### reaching_defs (13,153 functions, separable Gen/Kill)

| Config | DAG unique nodes | Per-query eval (`interp`) | Facts vs Default |
|---|---|---|---|
| Default (generic eval, Star iterated) | 1.000 | 1.000 | — |
| EAN (generic eval on reduced DAG) | 0.276 | 1.061 | 0 / 436,819 * |
| TranslAPA (closed-form fold, raw DAG) | 1.000 | 0.726 | **0 / 436,819** |
| **EAN ⊕ TranslAPA (fold reduced DAG)** | **0.276** | **0.337** | **0 / 436,819** |

\* EAN fact-correctness is validated by `real_eval.py` RQ1; the 0/436,819 column
here is the Default-vs-config differential recomputed by this harness.

- **Composition is exact.** 0 unequal IN lines out of 436,819 (reaching_defs) and
  622,719 (reachable) — folding the EAN-reduced DAG reproduces the framework's
  facts bit-for-bit, as both levers are semantics-preserving.
- **The levers compose (the headline).** EAN ⊕ TranslAPA (**0.337**) is the
  fastest config — **2.15×** faster than TranslAPA alone (0.726), **3.15×**
  faster than EAN alone (1.061), and **2.97×** faster than Default. EAN alone
  barely moves single-shot eval (1.061: generic tree-walk still expands the
  smaller DAG), and TranslAPA alone already wins by replacing Star iteration;
  stacking them multiplies.
- **Node reduction transfers into fold time (partial multiplicativity).**
  `fold(EAN DAG) / fold(raw DAG) = 0.464` while the node ratio is `0.276`
  (n=13,143 paired functions). The memoized fold cost tracks toward — but does
  not perfectly equal — the node count, because per-node cost is non-uniform
  (Union/Star and wider `BitVector`s are heavier than Atoms). The direction and
  magnitude confirm EAN's smaller graph makes the closed-form fold materially
  cheaper (>2× here).
- **Construction accounting.** Median construction for EAN ⊕ TranslAPA is 990 µs
  ≈ EAN saturation (883 µs) + mechanical extraction (110 µs), confirming the
  composed one-time cost is the sum of the two levers' one-time costs.

### reachable (17,021 functions, degenerate 1-fact lattice)

| Config | DAG unique nodes | Per-query eval (`interp`) | Facts vs Default |
|---|---|---|---|
| Default | 1.000 | 1.000 | — |
| EAN | 0.272 | 1.242 | 0 / 622,719 |
| TranslAPA | 1.000 | 9.150 | 0 / 622,719 |
| **EAN ⊕ TranslAPA** | **0.272** | **3.114** | **0 / 622,719** |

- Expected degenerate: with one boolean fact, generic eval is a trivial OR, so
  folding the whole DAG into 1-bit Gen/Kill elements loses badly (TranslAPA
  9.15×). **The composition lever still fires** — `fold(EAN)/fold(raw) = 0.341`,
  tracking the 0.272 node ratio — so EAN ⊕ TranslAPA (3.11×) is ~2.9× better than
  TranslAPA alone; it is simply swamped by the degenerate baseline. reaching_defs
  (and, for future B3, the IFDS clients) are the meaningful demonstrators.

### Reproducing

```
python3 docs/eval/translapa_eval.py run --suite coreutils   # adds td_ean_translapa
python3 docs/eval/translapa_eval.py agg --suite coreutils    # -> translapa_compose.csv
```

Correctness of the composed path is also locked by three unit tests
(`TranslAPADriver.Composed{Loop,Branch,NestedLoop}MatchesGeneric`): solve with
EAN enabled, fold the reduced DAG, assert node-by-node equality with the generic
interpreter on the raw DAG.

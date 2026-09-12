# Paper fill sheet — every [TBD] → value / prose draft

> Paste-ready content for `paper_apa_simp.pdf` (repo has no `.tex`; fill by hand).
> All numbers are from `docs/eval/*.csv` on the bc14 corpus (138 programs) +
> the synthetic corpus (75 subjects). Values are English for direct paste;
> `⟶` marks the source CSV. `[PROSE]` = suggested narrative wording.
> Last refreshed 2026-08-19 on the final e-graph (typed DSL + B-full + guarded
> expansion); correctness re-verified (0 divergent facts corpus-wide).

---

## Evaluation setup (§ Subjects and clients / Configurations)

- **Corpus** ⟶ `real_table4_corpus.csv`: **138** LLVM modules from **coreutils
  (109), an open-source set "open" (24), and SPEC (5)**. Together **171,575**
  functions, **4,480,094** LLVM instructions, **4,466,273** path-summary roots.
  Inclusion: every module that links; exclusion: none dropped outright —
  functions above the per-function instruction cap are skipped (368/coreutils,
  4,019/open, 895/spec skipped functions) and 13 modules hit a per-module
  timeout, both reported. Synthetic corpus: **75** subjects spanning branch
  depth, loop nesting, and cross-root reuse (families RepeatedPrefix/Suffix,
  CrossRootReuse, LoopFamily, NestedTrie, + 40 seeded-random).
- **Client algebras** ⟶ **6**: reachable, reaching definitions, live variables
  (Gen/Kill, distributive); possibly-uninitialized, copy-constant propagation
  (non-Gen/Kill); affine relations (relational, distributive). Profiled weights
  from the per-operator interpretation microbenchmark.
- **Differential tests** ⟶ `rq1_correctness.csv`: **3,000** on randomly
  generated expression DAGs and law profiles (6,210 total comparisons, 0
  unequal).
- **Default budget** ⟶ real_eval `EAN_BUDGET`: **B_N = 200,000 e-nodes,
  B_R = 30 rounds, B_T = 5 s**; B_M (match) and B_P (plateau) unbounded by
  default; RQ4 varies each.
- **Hardware/software**: Windows 11, MSYS2 ucrt64 GCC, LLVM 14.0.6 + Z3;
  state-elimination front-end. **5** measured runs per function (median), no
  warmup; interpretation amortization uses 50 per-query repeats. Configuration
  order randomized per subject×client. Timeout **180 s** (structural) / **45 s**
  (per-client correctness); max-func-insts cap 300 (structural) / 150 (peak,
  correctness). Runs exceeding the timeout contribute their finished functions
  (partial, marked).

---

## Table IV — corpus (families = suites) ⟶ `real_table4_corpus.csv`

| Family | Programs | Functions | Instructions | Roots | DAG nodes | Star nodes |
|---|---|---|---|---|---|---|
| coreutils | 109 | 17,023 | 623,215 | 623,186 | 24,050,362 | 2,920 |
| open | 24 | 129,876 | 3,039,428 | 3,025,994 | 114,106,682 | 11,623 |
| SPEC | 5 | 24,676 | 817,451 | 817,093 | 34,691,072 | 6,904 |
| Synthetic | 75 subj | — | — | 143 | 1,473 | 45 |
| **Total (real)** | **138** | **171,575** | **4,480,094** | **4,466,273** | **172,848,116** | **21,447** |

---

## Table VI — final complexity vs Default (geo-mean) ⟶ `real_table6_complexity.csv`

| Config | Unique nodes | DAG edges | Tree size | Sequence | Stars | Sharing |
|---|---|---|---|---|---|---|
| Greedy | 0.397 | 0.257 | 0.998 | 0.247 | 1.000 | 4.39→18.3 |
| Order | 0.367 | 0.226 | 1.078 | 0.220 | 0.993 | 4.39→115.2 |
| EAN | 0.395 | 0.256 | 0.999 | 0.245 | 1.000 | 4.39→18.8 |
| Order+EAN | 0.352 | 0.212 | 1.037 | 0.205 | 0.994 | 4.39→111.6 |

- Unique-node reduction: **geo-mean 0.35–0.40× (up to ~0.18× on large
  functions)**; **Sequence 0.20–0.25×** is the dominant driver (prefix/suffix
  factorization). Greedy≈EAN on per-function batches (competing forms add <1%);
  Order+EAN best via the monotone guard.

---

## Table VII — performance vs Default (geo-mean) ⟶ `real_table7_performance.csv`

| Config | Generation | Normalization | Interpretation | End-to-end | Peak RSS | Timeouts |
|---|---|---|---|---|---|---|
| Greedy | 1.301 | 6.03 | 1.499 | 6.609 | 1.316 | 20 |
| Order | 1.352 | — | 1.051 | 1.333 | 1.118 | 13 |
| EAN | 1.312 | 6.66 | 1.549 | 7.171 | 1.334 | 22 |
| Order+EAN | 1.740 | 5.63 | 1.554 | 6.671 | 1.288 | 19 |

- Interpretation on these clients is µs-cheap while saturation is ms; EAN is an
  **IR-size** optimization here (end-to-end ~6.7×), not a speed one.

## Table VII — second client (reaching_defs, coreutils) ⟶ `real_table7_reaching_defs.csv`

| Config | Generation | Normalization | Interpretation | End-to-end | Peak RSS | Timeouts |
|---|---|---|---|---|---|---|
| Greedy | 1.088 | 5.31 | 1.055 | 3.394 | 1.194 | 22 |
| Order | 1.378 | — | 1.015 | 1.172 | 1.085 | 16 |
| EAN | 1.106 | 6.32 | 1.069 | 3.833 | 1.199 | 22 |
| Order+EAN | 1.504 | 4.67 | 1.019 | 3.267 | 1.162 | 12 |

(13,237 functions.) **Per-client trend**: reaching_defs has a costlier baseline
than reachable, so the same fixed EAN saturation cost is a **smaller** multiple —
EAN end-to-end drops from **7.17× (reachable) to 3.83× (reaching_defs)**, and
Order+EAN to **3.27×**. Interpretation stays ≈1.0× (non-memoized tree eval).
This is the trend that, on a memoized expensive client (affine), flips to a net
win (interp 0.454×, break-even K=0).

---

## Table VIII — ablation vs full EAN ⟶ `table8_ablation.csv`

| Variant | Final nodes | End-to-end | Note |
|---|---|---|---|
| No factorization | 1.163 | 0.554 | factorization = dominant node reducer |
| No star rules | 1.00 | 0.953 | sliding re-associates, no node change |
| No guarded expansion | 1.00 | 0.930 | node-neutral on this corpus; fires + adds work |
| No phase schedule | 1.00 | 1.146 | unscheduled slower, same quality |
| Uniform tree cost | 1.00 | 0.874 | cost weights only affect selection |
| Profiled tree cost | 1.00 | 0.857 | " |

---

## RQ1 — Correctness & IR quality

- **Answer to RQ1.** EAN preserves **all** client results and reduces unique
  expression nodes to **geo-mean 0.40× (real EAN) / 0.35× (Order+EAN) / 0.86×
  (synthetic)**. ⟶
  `real_rq1_correctness.csv`: across reachable (1.65M IN lines), reaching-defs
  (0.71M), liveness (1.06M), uninitialized (1.65M), constant-prop (0.16M),
  **0 unequal, 0 missing roots, 0 semantic timeouts** (constant-prop: 8
  slow-client crashes, excluded). Its advantage over Greedy is largest when
  competing rewrite directions or cross-root sharing matter (rare on
  per-function batches — an honest negative for these clients).
- **Law-gating validation** ⟶ `real_rq1_kleene_divergence.csv`: over-declaring
  the full-Kleene profile on non-distributive clients diverges — reachable
  435/986,929 (**0.04%**), reaching-defs 19,050/803,841 (**2.4%**), liveness
  23,009/811,377 (**2.8%**) — while safe-minimal stays 0. This quantifies R1
  (law-gated admissibility) on real code.
- **[PROSE] Inspecting one optimization** ⟶ case study (dirname,
  reaching-defs, function `emit_ancillary_info`): Default emits **3,592** DAG
  nodes; the reaching-definition joins repeatedly share a common prefix `P`.
  EAN exposes the equality `(P·c) ⊕ (P·d) = P·(c ⊕ d)` (variadic prefix
  factorization), extracts the factored form, and reduces the retained DAG to
  **654** nodes (**0.182×, −82%**). This connects the aggregate reduction to the
  mechanism rather than to a black-box outcome.

---

## RQ2 — End-to-end performance

- **Answer to RQ2.** Including optimization overhead, EAN changes end-to-end
  time by **~6.7×** and peak RSS by **~1.3×** (⟶ Table VII); it is profitable
  only for **amortized/memoized multi-query workloads**, while small or
  cheap-interpretation workloads need the invocation gate.
- **Break-even by size** ⟶ `real_rq2_breakeven.csv`: EAN/Default end-to-end
  rises **6.5× (0–50 nodes) → 12.1× (>5k)** — expressions do not break even in
  a single shot at any size on these µs-interpretation clients.
- **Amortized** ⟶ `real_rq2_amortized.csv`: under a memoizing interpreter, the
  per-query interpretation is **6.99× faster** with EAN; EAN breaks even after
  **K ≈ 1,106** queries.
- **Which client gains most/least** (synthesized ⟶ `affine_table7.csv`,
  `translapa_compare.csv`, Table VII): **affine gains most** — with a memoizing
  transformer interpreter, EAN's node reduction (0.447×) transfers almost
  linearly into interpretation time (**0.454×**, i.e. ~2.2× faster), and
  break-even is **K = 0** (⟶ `affine_breakeven.csv`: one-time norm 49 s vs
  interpretation saved 2,106 s). **reachable / reaching-defs gain least**
  (interp 1.0–1.5×) because their per-query interpretation is µs-cheap and
  non-memoized, so structural minimality does not convert to time. This
  contrast shows structural minimality and client cost are related but not
  identical.
- **Gate** ⟶ `real_rq2_gate.csv`: a raw-node gate at min-nodes = 200 runs EAN
  on **28% of functions** (skips 72% of batches) and changes total time by only
  **~−1.9%** (7.36→7.22) — the skipped batches are the cheap small ones, so the
  gate removes overhead almost for free; its mispredictions are negligible
  because EAN gives no single-shot interpretation win on these clients anyway.
- **[PROSE] Memory**: peak RSS ~1.3× (Table VII); the e-graph raises transient
  memory during normalization while the exported DAG is smaller — the maximum
  occurs in the saturation phase.

---

## RQ3 — Complementarity with elimination ordering ⟶ `rq3_complementarity.csv`

- **Answer to RQ3.** Ordering controls construction peaks; EAN controls
  retained IR and downstream interpretation. **Order+EAN improves final unique
  nodes to 0.739× of Default and 0.846× of Order alone.** ⟶ real Table VI:
  Order 0.367, EAN 0.395, Order+EAN 0.352 (real corpus); synthetic
  complementarity subjects give 0.874 / 0.884 / 0.739.
- The two gains have **Spearman ρ = −0.22** (weak negative → complementary, not
  redundant); parity **358/358** exact. Ordering dominates on dense/hub graphs
  (predecessor–successor product drives the peak); EAN dominates on
  repeated-factor / multi-root structure. ⟶ `real_rq3_peak.csv`: cost-aware
  ordering raises peak construction nodes ~1.03–1.08× here (it trades a slightly
  higher peak for a smaller final IR).

---

## RQ4 — Rewrites, budgets, extraction ⟶ `rq4_budget.csv`, `table8_ablation.csv`

- **Answer to RQ4.** **Factorization** exposes the main equivalences (removing
  it costs the largest node regression, 1.163×); **guarded phase scheduling**
  contains search growth (unscheduled is 1.15× slower for no quality gain); and
  **reuse-aware profiled extraction** is the extractor of record (competing tree
  extractors match on these per-function batches).
- **Budget knee** ⟶ `rq4_budget.csv`: single-round families (RepeatedSuffix
  167→47, CrossRootReuse 32→31) **saturate at round 1**. Multi-round families
  need depth-many rounds: **NestedTrie(6) 156→125 over 6 rounds** while peak
  e-nodes grow 132→363 (**2.7×**), saturating at round 7; NestedTrie(7) 316→253
  over 8 rounds, peak 260→795 (**3.05×**). Extracted cost keeps improving up to
  ≈ tree depth while normalization memory grows ~3×; this knee motivates the
  default round budget.

---

## Still genuinely pending (not closeable from current data)

- ~~Synthetic Table VI **Greedy** row~~ **DONE** ⟶ `table6_ir_quality.csv`:
  Greedy unique_nodes **0.952**, edges 0.892, tree 0.800, sequence 0.772,
  sharing 1.589→1.267 (vs EAN 0.857 / 0.727 / 0.679 / 0.517 / →1.193). The
  Greedy>EAN gap on the synthetic path-expr corpus quantifies the value of
  retaining competing forms for reuse-aware extraction (absent in the
  per-function real batches, present here).
- ~~Full second-client **Table VII** (reaching_defs)~~ **DONE (coreutils)** ⟶
  `real_table7_reaching_defs.csv` (see the multi-client note under RQ2).
- **Incremental-analysis** scenario (TranslAPA's 21×/2× angle) for EAN — not
  measured; RQ2 covers the amortized-query angle instead.
- The paper's remaining pure-prose cells (motivation sentences, threats to
  validity) — authoring, not data.

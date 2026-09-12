# EAN / APA 项目总结（Project Summary）

> 生成日期：2026-08-18　|　范围：LOTUS 中的代数程序分析（APA）优化器 **EAN** + 外部强基线 **TranslAPA**
> 本文档以**结果数据**为核心，附**数据来源映射**与**文件目录**，供论文填表与复现。
> 数据来源：`docs/eval/*.csv`（真实 LLVM 语料 bc14 + 合成语料），单测：`tests/unit/Dataflow/APA/`。

---

## 1. 项目概述

**EAN（Equality-saturation Algebraic Normalizer）**：在 APA 的"路径表达式**构造**"与"**解释**"之间插入的可选优化阶段——把每节点路径表达式 DAG 当作可优化 IR，导入 law-参数化 e-graph，做受守卫/预算的等式饱和（ACI 规范化、前后缀因式分解、星规范化/滑动、guarded expansion），再按客户端代价模型做 reuse-aware 批抽取，导回工厂。正确性相对客户端声明的 **law profile**（左/右分配、湮灭、Kleene 星…），失败时回退原始 batch（根保持 I3）。

**TranslAPA baseline**：复刻 OOPSLA 2026《Mechanically Translating IDA to APA》的机械翻译，作为**外部强基线**。复用同一消元前端产出的路径表达式 DAG，只**换解释器**为闭式 Gen/Kill 半环折叠（星 O(1) 无迭代）。

**评测五配置**：Default（基线消元+hash-cons）/ Greedy（+确定性一遍化简）/ Order（代价感知消元序）/ EAN / Order+EAN。**五客户端**：reachable、reaching_defs、liveness（分配式）+ uninitialized、constant_prop（非分配式）+ **affine relation analysis**（分配式，R1 正面案例）。

---

## 2. 核心结果（数据）

### 2.1 语料规模（Table IV）
| 族         | 程序      | 函数          | 指令            | 路径根           | DAG 节点          | 星节点        | 超时     |
| --------- | ------- | ----------- | ------------- | ------------- | --------------- | ---------- | ------ |
| coreutils | 109     | 17,023      | 623,215       | 623,186       | 24,050,362      | 2,920      | 0      |
| open      | 24      | 129,876     | 3,039,428     | 3,025,994     | 114,106,682     | 11,623     | 12     |
| spec      | 5       | 24,676      | 817,451       | 817,093       | 34,691,072      | 6,904      | 1      |
| **合计**    | **138** | **171,575** | **4,480,094** | **4,466,273** | **172,848,116** | **21,447** | **13** |

### 2.2 RQ1 正确性 + 律法可采纳性（headline）
- **safe-minimal EAN（及 Greedy）在真实语料上保持全部客户端结果**：跨 5 客户端 **0 unequal**（reachable 165 万 / reaching_defs 71 万 / liveness 106 万 / uninitialized 165 万 / constant_prop 16 万 IN 行）。
- **律法门实证（过度声明不 sound）**：用 full-Kleene（含右分配+滑动）跑非分配式客户端 → **reachable 435/986,929（0.04%）、reaching_defs 19,050/803,841（2.4%）、liveness 23,009/811,377（2.8%）不一致**；safe-minimal 全 0。→ 在真实语料上量化验证论文 **R1（law-gated admissibility）**。
- **affine 是 R1 正面案例**：affine **分配式** → full-Kleene **SOUND**，`affine_rq1.csv` 显示 safe 与 kleene **双双 0 unequal**（9,635 函数 / 176,003 行）。与 reachable/liveness 的 2.4–2.8% 偏差直接对照，证明**可采纳性取决于客户端代数**。

### 2.3 Table VI — IR 复杂度（相对 Default 几何均值，<1 为缩减）
| 配置            | 唯一节点      | DAG 边     | Tree  | Sequence  | Stars | Sharing(前→后) |
| ------------- | --------- | --------- | ----- | --------- | ----- | ------------ |
| Greedy        | 0.397     | 0.257     | 0.998 | 0.247     | 1.000 | 4.39→18.3    |
| Order         | 0.367     | 0.226     | 1.078 | 0.220     | 0.993 | 4.39→115.2   |
| EAN           | 0.395     | 0.256     | 0.999 | 0.245     | 1.000 | 4.39→18.8    |
| **Order+EAN** | **0.352** | **0.212** | 1.037 | **0.205** | 0.994 | 4.39→111.6   |
|               |           |           |       |           |       |              |

→ 唯一节点降到 ~0.35–0.40×（几何均值省 60–65%）；**Sequence(concat) 降到 ~0.20–0.25× 为主贡献**（前后缀因式分解压缩共享）。Order+EAN 靠单调守卫叠加，永不更差。

### 2.4 Table VII — 性能（相对 Default 几何均值）
| 配置 | Generation | Normalization | Interpretation | End-to-end | Peak RSS | Timeouts |
|---|---|---|---|---|---|---|
| Greedy | 1.301 | 6.03 | 1.499 | 6.609 | 1.316 | 20 |
| Order | 1.352 | — | 1.051 | 1.333 | 1.118 | 13 |
| EAN | 1.312 | 6.66 | 1.549 | 7.171 | 1.334 | 22 |
| Order+EAN | 1.740 | 5.63 | 1.554 | 6.671 | 1.288 | 19 |

→ **诚实结论**：这些 dataflow 客户端**解释本身 µs 级极廉**、EAN 饱和 ms 级，故在此语料 **EAN 是"IR 体积/保留表示"优化,而非速度优化**（end-to-end 慢 ~6.7×）。此表用退化的 reachable 客户端。

**第二客户端 reaching_defs**(coreutils, 13,237 函数;`real_table7_reaching_defs.csv`):EAN gen 1.11 / norm 6.32 / interp 1.07 / **end2end 3.83** / rss 1.20;Order+EAN **end2end 3.27**。→ **跨客户端趋势**:reaching_defs 基线更贵,固定的 EAN 饱和开销占比更小,故 EAN end-to-end 从 **7.17×(reachable) 降到 3.83×(reaching_defs)**;interp 仍 ~1.0×(非记忆化)。这条趋势在**记忆化的昂贵客户端 affine 上翻转为净赚**(见 2.5)。

### 2.5 RQ2 — 摊还 / break-even
- **按 raw 节点分桶**（`real_rq2_breakeven.csv`）：EAN/Default end-to-end 6.5×(0–50 节点) → 12.1×(>5k)。
- **摊还（memoizing 投影, `real_rq2_amortized.csv`）**：per-query 解释 **6.987× 加速**，break-even **K=1106** 次查询。即"多查询摊还"场景 EAN 才回本——APA 的目标场景。
- **affine + 记忆化解释器（决定性正面案例, `affine_breakeven.csv`）**：**break-even K=0**（norm 49.3s vs 解释节省 2,106s），memo 下解释时间比 **0.454**（≈节点比 0.447）→ **昂贵+记忆化解释时,EAN 第一次解释即净赚**。

### 2.6 RQ3 — 与消元序的互补性（`rq3_complementarity.csv`）
Spearman(ordering vs EAN 增益) = **−0.22**（弱负相关 → 两者**互补**，攻不同结构）；Order+EAN 0.739 vs Default、0.846 vs Order；**parity 358/358 逐点相等**。→ Order 控构造峰值、EAN 控保留 IR。

### 2.7 Table VIII — 消融（相对 full EAN；`table8_ablation.csv`）
| 变体 | Final nodes | End-to-end | 结论 |
|---|---|---|---|
| No factorization | **1.163** | 0.554 | **因式分解=节点缩减主导机制** |
| No star rules | 1.00 | 0.953 | 滑动不改节点数 |
| No guarded expansion | 1.00 | 0.930 | 展开在语料上节点中性(诚实中性) |
| No phase schedule | 1.00 | **1.146** | 无调度更慢质量不变(印证"白费预算") |
| Uniform / Profiled tree cost | 1.00 | 0.87 | 代价权重只影响等价形选择 |

### 2.8 TranslAPA 基线 + EAN⊕TranslAPA 组合（`translapa_*.csv`, coreutils）
| 指标(reaching_defs, 13,153 函数) | 值 |
|---|---|
| EAN 唯一节点比 | 0.276 |
| TranslAPA 闭式折叠 per-query 解释比 | **0.726**（击败通用星迭代） |
| **EAN⊕TranslAPA(折叠 EAN 精简 DAG)** | **0.337**（比 TranslAPA 单用快 2.15×） |
| 相乘验证 fold(EAN)/fold(raw) | 0.464 vs 节点比 0.276 |
| 事实一致 | **0 / 436,819** |

→ 两个正交杠杆(EAN 减节点 × TranslAPA 闭式星)**可叠加**;组合精确保义。

### 2.9 e-graph 引擎优化（`docs/eval/egraph_speedup.md`）
父结点工作队列抽取 + 增量 rebuild + typed 节点 → 富客户端(reaching_defs) **EAN 归一化 1.72×(−41.8%)**;A+B-safe 部分**逐字节等价**,typed DSL+B-full 仅 tie-break 微移;**全语料正确性重验 0 偏差**。

---

## 3. 数据来源映射（CSV → 论文格）

| 论文表/RQ | CSV 文件 | 语料 |
|---|---|---|
| Table IV 语料 | `real_table4_corpus.csv` / `table4_corpus.csv`(合成) | bc14 / 合成 |
| Table VI IR 复杂度 | `real_table6_complexity.csv` / `table6_ir_quality.csv` + `table6_order_rows.csv`(合成) | bc14 / 合成 |
| Table VII 性能 | `real_table7_performance.csv`(reachable) + `real_table7_reaching_defs.csv`(第二客户端) | bc14 / coreutils |
| Table VIII 消融 | `table8_ablation.csv` | 合成 |
| RQ1 正确性 | `real_rq1_correctness.csv` / `rq1_correctness.csv`(合成) | bc14 / 合成 |
| RQ1 律法门 | `real_rq1_kleene_divergence.csv` | bc14 |
| RQ2 break-even/gate/摊还 | `real_rq2_breakeven.csv` / `_gate.csv` / `_amortized.csv` | bc14 |
| RQ3 互补性 | `rq3_complementarity.csv` / `real_rq3_peak.csv` / `rq3_peak.csv` | 合成 / bc14 |
| RQ4 预算 | `rq4_budget.csv` | 合成 |
| affine（R1 正面案例 + memo RQ2） | `affine_rq1.csv` / `affine_table7.csv` / `affine_breakeven.csv` / `affine_coverage.csv` | bc14 |
| TranslAPA 三方 + 组合 | `translapa_compare.csv` / `translapa_compose.csv` | coreutils |
| e-graph 引擎提速 | `docs/eval/egraph_speedup.md` | coreutils 子集 |

**语料位置**：bc14 = `D:/Code/lotus/bc14/bc14/{coreutils,open,spec}`；原始工具输出缓存 = `docs/eval/raw/{suite}/`。

---

## 4. 文件目录

### 4.1 EAN 优化器 —— `include/Dataflow/APA/EAN/`
| 文件 | 职责 |
|---|---|
| `PathLang.h` | **typed 路径表达式节点**（enum PathKind + uint32 atom；替代旧 stringly-typed SymbolLang） |
| `Import.h` / `Export.h` | 路径表达式工厂 ↔ e-graph 桥接 |
| `Canonical.h` | 规范化 e-node 构造（ACI join / seq / star 化简，不变式 I2） |
| `Factorize.h` | 前后缀因式分解（Factor 阶段） |
| `Star.h` | 星滑动 `(a·b)*·a=a·(b·a)*`（Star 阶段） |
| `Expand.h` | **guarded expansion**（Explore 阶段，因式分解逆向 + 对齐守卫） |
| `Saturate.h` | 相位化/预算化饱和驱动（+ 无调度模式开关） |
| `BatchExtract.h` | **reuse-aware 批抽取**（Eq.5 目标 + 父结点工作队列松弛） |
| `LawProfile.h` / `CostModel.h` / `CostFn.h` / `Budget.h` / `ExtractOptions.h` | 律法档案 / 代价模型 / 预算 / 抽取选项 |
| `DagStats.h` / `AtomTable.h` / `Greedy.h` / `EAN.h` | DAG 指标 / atom 表 / 一遍化简 / 顶层入口 |

### 4.2 TranslAPA 基线 —— `include/Dataflow/APA/Baseline/TranslAPA/`
`GenKillSemiring.h`（闭式 Gen/Kill 半环）· `AtomTranslator.h`（机械抽取，separable 快路径）· `FoldInterpreter.h`（记忆化折叠）· `Driver.h`（`foldFillGenKill` + 分列计时）。

### 4.3 共享 e-graph —— `include/Solvers/EGraph/`
`EGraph.h`（核心，增量 rebuild/congruence）· `Extract.h`（工作队列抽取器）· `EClass.h`/`UnionFind.h`/`Analysis.h`/`Language.h`/`RecExpr.h`/`Subst.h` · `Runner.h`/`Rewrite.h`/`Pattern.h`/`PatternMachine.h`（rewrite/matching，EAN 不用，独立测试用）。

### 4.4 客户端 / 域 / 工具 —— `lib/Dataflow/APA/`
客户端封装 `Analyses/Intra/Intra{Reachable,ReachingDefinitions,LiveVariables,UninitVariables,ConstantPropagation,AffineEqualities,...}.cpp`（含 `runIntraElim*` 与 `runIntraTranslApa*`）；域 `Domains/AffineRelationDomain.cpp`；CLI 工具 `tools/dataflow/lotus-dfa-apa.cpp`（`--ean/--greedy/--ordering/--interp={generic|translapa}/--memo-interp/--measure-peak` 等）。

### 4.5 测试 —— `tests/unit/Dataflow/APA/`
- EAN：`EAN/{Roundtrip,Factorize,Saturate,Star,BatchExtract,Expand,Greedy,SolverWiring,InterWiring,AffineEan,EvalRq1,EvalRq3,EvalExport}Test.cpp`（其中 `BatchExtractTest` 含 worklist-vs-naive 差分；`EvalExportTest` 产出合成语料 CSV）。
- TranslAPA：`Baseline/TranslAPA{GenKill,Driver}Test.cpp`。

### 4.6 评测与文档 —— `docs/`
- 驱动：`eval/real_eval.py`（真实语料 Table VI/VII/RQ1-3）· `eval/translapa_eval.py`（三方+组合）· `eval/affine_eval.py`（affine + memo）· `eval/table7_reaching_defs.py`（第二客户端 Table VII）· `eval/rerun_step4.sh`（一键重跑）。
- 结果：`eval/RESULTS.md`（真实+合成主结果 + CSV→格映射）· `eval/RESULTS_TranslAPA.md`（TranslAPA 三方+组合）· `eval/egraph_speedup.md`（引擎提速 + 论文注脚草稿）· `eval/PAPER_FILL.md`（逐 [TBD] 填充值 + RQ 叙述草稿）· 本文件 `PROJECT_SUMMARY.md`。
- 设计：`EAN_项目计划书.md` · `TranslAPA_Baseline_设计方案.md`。

---

## 5. 关键工程结论与局限（诚实记录）

1. **EAN 是 IR-体积优化,速度收益依场景**：解释廉价的客户端(reachable 等)上 EAN end-to-end 慢 ~6.7×；只有在**解释昂贵 + 记忆化 + 多查询摊还**（affine：K=0；或投影 K≈1106）时才转化为速度净赚。
2. **律法可采纳性取决于客户端代数**：affine 分配式 → kleene sound（0 偏差）；reachable/liveness 非分配式 → kleene 2.4–2.8% 偏差。safe-minimal 普适安全。
3. **EAN 与消元序互补**（Spearman −0.22），Order+EAN 最优。
4. **两个正交杠杆可叠加**：EAN⊕TranslAPA 折叠精简 DAG 比单用快 2.15×。
5. **局限**：大函数受域固有开销(affine O(vars³))/前端消元 O(n³) 限制而超时(全语料 13 个程序级超时、affine 137 程序尾部截断但部分捕获)；inter path-summary 不可扩展（已如实记录，未纳入主语料）；Table VI 合成 Greedy 行、真实 LLVM Table IV 分族仍可补。
6. **e-graph 引擎**：typed DSL + 增量 rebuild + 工作队列抽取带来 1.72× 归一化提速且保义；guarded expansion / 无调度模式已实现（Table VIII 补全）。

---

## 6. 复现

```bash
# 构建
cmake --build build --target lotus-dfa-apa apa_tests -j 8
# 单测（apa_tests 171/172，唯一失败为既有无关 EliminationTest）
./build/bin/tests/apa_tests.exe
# 合成语料 CSV（Table VI/VIII/RQ1/RQ4）
EAN_EVAL_OUT=docs/eval ./build/bin/tests/apa_tests.exe --gtest_filter='*EvalExport*'
# 真实语料全量（Table VI/VII/RQ1-3 + translapa + affine）
bash docs/eval/rerun_step4.sh          # 或分别 python real_eval.py run/agg 等
```
> 注：`real_eval.py` 等按 `docs/eval/raw/` 缓存,可续跑；仅 EAN 配置受引擎改动影响（非 EAN 缓存可复用）。

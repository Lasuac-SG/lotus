# EAN 评测数据导出 — 论文 §IV 填表指南

本目录下的 CSV 是 **EAN 评测 harness** 自动产出的**有效数据**（可复现、可回归）。
生成器：`tests/unit/Dataflow/APA/EAN/EvalExportTest.cpp`；统计口径：`include/Dataflow/APA/EAN/DagStats.h`。

> 复现命令（隔离构建，无需 LLVM）：
> ```bash
> g++ -std=c++17 -O2 -static -static-libstdc++ -static-libgcc \
>   -I include -DLOTUS_EGRAPH_ENABLE_JSON=0 -DLOTUS_EGRAPH_ENABLE_DOT=0 \
>   tests/unit/Dataflow/APA/EAN/EvalExportTest.cpp \
>   -I /d/Env/MSYS2/ucrt64/include -L /d/Env/MSYS2/ucrt64/lib -lgtest -lgtest_main \
>   -o eval_export.exe
> EAN_EVAL_OUT="$(pwd)/docs/eval" ./eval_export.exe
> ```
> ⚠️ 必须 `-static-libstdc++`：否则运行时会误载 Git 自带的 mingw64 `libstdc++-6.dll`（msvcrt ABI，与 ucrt 冲突），在 `ofstream` 构造处段错误。

## 方法学（对齐论文 §IV-A/B）

- **比较对象**：client **语义事实**（布尔矩阵 Kleene 代数解释值），不是指针/语法——因为 EAN 有意改变表示。
- **oracle**：`BoolKleene.h`（3×3 布尔矩阵：join=OR，seq=布尔矩乘，star=自反传递闭包）。它满足**全部**律法，故可用 full Kleene profile 做 sound 差分。
- **聚合**：Table VI/VIII 用**每-subject 比值的几何均值**（`<1` 表示缩减）；Sharing 报绝对 before/after。
- **语料**：75 个合成 subject，覆盖 RepeatedPrefix/Suffix、CrossRootReuse、LoopFamily、NestedTrie、随机种子 DAG。这对应论文 Table IV 的 **Synthetic** 行。

---

## 已可填入论文的单元格

### 1. Table IV（语料，归一化前）→ `table4_corpus.csv`
论文 Synthetic 行填：**Roots=143，DAG nodes=1473，Star nodes=45**（75 个 subject）。
各族明细在 CSV 里（可用于正文举例）。真实 LLVM 两族（family 1/family 2）**仍 [TBD]**，需 Phase 3。

### 2. Table VI（IR 复杂度，相对 Default 的几何均值）→ `table6_ir_quality.csv`
**EAN 行**可直接填：

| Unique nodes | DAG edges | Tree size | Sequence | Stars | Sharing (before→after) |
|---|---|---|---|---|---|
| **0.857** | **0.727** | **0.679** | **0.517** | 1.00 | **1.589 → 1.193** |

解读（可写入正文）：EAN 几何均值把唯一节点降到 **0.857×**（≈少 14%），其中 **Sequence（concat）节点降到 0.517×** 是主贡献——因为因式分解把共享前/后缀合并；Stars=1.00 是因为滑动只重结合、不改节点数。Sharing 由 1.589 降到 1.193，印证论文那句"tree size 单独会误判表示"。
- Greedy / Order / Order+EAN 三行 **[TBD]**：需先实现 Greedy 一遍化简器与 Order 代价感知消除次序（Phase 3）。

### 3. RQ1 正文数字 → `rq1_correctness.csv`
- 「Across **6210** comparisons, EAN produces **0** unequal facts, **0** missing roots, **0** semantic timeouts.」
- 「We also run **3000** differential tests on randomly generated DAGs and law profiles.」（其中 0 不一致）
- 「EAN reduces unique nodes by **geo-mean 0.857×**（即 −14.3%）」。
- **Answer to RQ1**：EAN preserves **all（100%）** client results.

### 4. Table VIII（消融，相对 full EAN）→ `table8_ablation.csv`

现含 `end2end_ratio_vs_fullEAN` 列(合成语料墙钟,>1=比 full 慢、<1=快;真实计时见 Table VII)。

| Variant | Final nodes | End-to-end | 说明 |
|---|---|---|---|
| **No factorization** | **1.163**（多 16%）| 0.554 | 关掉左右分配律——**节点缩减的主导机制**;去掉后更快但更胖 |
| No star rules | 1.00 | 0.953 | 滑动只重结合,不改节点数 |
| **No guarded expansion** | **1.00** | **0.930** | Explore 已落地(`Expand.h`);合成语料上展开**不改最终节点**(共享已由 factorization 捕获),但会触发并加工作 → 去掉略快。诚实中性结果 |
| **No phase schedule** | **1.00** | **1.146** | 无调度模式已落地(`scheduled=false`);**更慢、质量不变** ⇒ 印证论文"无约束搜索在等价形式上白费预算" |
| Uniform tree cost | 1.00 | 0.874 | 代价权重只影响等价形选择,不改本族节点数 |
| Profiled tree cost | 1.00 | 0.857 | 同上 |
| Reuse-aware / Profiled-tree | 1.00 | — | 安全网取等号:本族哈希共享已被树抽取捕获 |

可写入正文：**factorization** accounts for the largest node reduction；star/cost/reuse 在这些族上不改节点数，其价值属 RQ2 的解释时间维度。

### 5. RQ4 预算拐点 → `rq4_budget.csv`
单轮饱和族（RepeatedSuffix/CrossRootReuse）在**第 1 轮**即饱和。
**多轮嵌套族 NestedTrie** 展示清晰的膝点：

| NestedTrie(6) | raw | r1 | r2 | r3 | r4 | r5 | **r6(饱和)** | r8+ |
|---|---|---|---|---|---|---|---|---|
| final unique nodes | 156 | 155 | 154 | 151 | 145 | 133 | **125** | 125 |
| peak e-nodes | – | 132 | 170 | 215 | 273 | 325 | **333** | 333 |

可写入正文：随预算增大，抽取节点数持续下降到第 6 轮后**平台化（125）**，而峰值 e-nodes 单调升到 333——即"**收益递减的膝点同时伴随瞬时内存增长**，这正是默认预算的动机"。NestedTrie(7) 同形：316→253（7 轮），peak→733。

### 6. RQ3 互补性（Order 2×2）→ `table6_order_rows.csv` / `rq3_peak.csv` / `rq3_complementarity.csv`
方法：合成 CFG 语料（Hub/Dense/DiamondChain/NestedLoop/Random，33 subject）过**真实求解器**，分布式 ReachDomain，4 配置 {Default,Order}×{no-EAN,EAN}。**parity 358/358 逐点相等**（枢轴序 + EAN 保结果）。

**Table VI 的 Order / EAN(CFG语料) / Order+EAN 三行**（几何均值 vs Default）：

| Config | Unique nodes | DAG edges | Tree size | Sequence | Stars | Sharing→ |
|---|---|---|---|---|---|---|
| Order | 0.874 | 0.859 | 0.520 | 0.893 | 0.640 | 49.1→15.1 |
| EAN | 0.884 | 0.823 | 0.654 | 0.784 | 1.00 | 49.1→38.7 |
| **Order+EAN** | **0.739** | **0.671** | **0.374** | **0.637** | 0.640 | 49.1→14.5 |

**RQ3 正文（核心）**：
- **Order+EAN (0.739) 优于单独 Order (0.874) 与 EAN (0.884)** → 互补；`orderEAN_vs_order_final=0.846`（Order 之上 EAN 再降 15%）。
- **Spearman ρ(ordering 收益, EAN 收益) = −0.221** → 两者作用在不同 subject 上（负相关=互补，非冗余）。可直接填"the two gains have Spearman ρ=[−0.22]"。
- **peak construction nodes 降幅**（`rq3_peak.csv`，Order vs Default）：**NestedLoop 0.385×**、**Random 0.720×**（基线序糟糕的环状/不规则图 ordering 主导）；Hub/Dense/DiamondChain=1.00（前馈 hub 的 entry-first 基线已够好、完全图已饱和）。印证"ordering dominates on dense/hub/irregular where Eq.1 product drives the peak"，且节点峰值比 peakFill 边代理更能体现膨胀（NestedLoop 节点 0.385 vs 边 9.5→8）。
- Order 单独把 final nodes 只降到 0.874（"reduces final nodes by only a little"），大头留给 EAN——与论文分工叙事一致。

---

## 尚不能填（需后续里程碑，**未伪造**）

| 论文位置 | 缺什么 | 阻塞于 |
|---|---|---|
| Table VI 的 **Greedy** 行 | Greedy 配置 | 造 **Greedy** 一遍化简器 |
| Table VII 全表（RQ2 计时）| Generation/Normalization/Interpretation/End-to-end/RSS | 需**真实 client + LLVM 语料**；合成计时无代表性 |
| Table IV 真实 LLVM 两族 | 真实 bitcode 语料 | `-elim-ean` pass 开关 + 各 `runIntraElim*` 入口按 client 配档案 |
| Table VII 的 Order/Order+EAN 计时行 | 真实计时 | 同 RQ2 |
| Table VIII 的 guarded-expansion / phase-schedule 行 | 这两个机制 | Explore 阶段与可切换调度（当前推后）|

> **已解锁（本轮 M-Order + RQ3 harness）**：Table VI 的 Order/Order+EAN 行、RQ3 全部结构化指标（peak 构造节点、final 降幅、Spearman 互补性）。

---

## 文件清单
| 文件 | 对应论文 |
|---|---|
| `table4_corpus.csv` | Table IV（Synthetic 行 + 各族明细）|
| `table6_ir_quality.csv` | Table VI（EAN 行，手搓 path-expr 语料）|
| `table6_order_rows.csv` | **Table VI 的 Order / EAN(CFG) / Order+EAN 行** |
| `rq1_correctness.csv` | RQ1 正文计数 + Answer to RQ1 |
| `rq3_peak.csv` | **RQ3 peak 构造节点（Order vs Default，按族）** |
| `rq3_complementarity.csv` | **RQ3 Spearman + Order+EAN headline 比值** |
| `table8_ablation.csv` | Table VIII（已实现机制的行）|
| `rq4_budget.csv` | RQ4 预算扫描 / 膝点 |

---

# 真实 LLVM 语料评测（bc14）— 补充论文 Table IV/VI/VII 真实族 + RQ1/RQ2/RQ3

数据由 `real_eval.py`（驱动 `lotus-dfa-apa` 跑 bc14）产出，CSV 前缀 `real_`。

## 方法学（对齐论文 §IV-a/b，如实记录）
- **语料**：`bc14/{coreutils,open,spec}` = 138 程序（`__MACOSX` 资源叉已排除）。
- **配置**：Default / Order(cost-aware) / EAN / Order+EAN，`--elim-method=state`。
- **EAN 档案 = safe-minimal（仅左分配律）**——见下方 RQ1，这是唯一对所有 client **sound**（保结果）的档案；full Kleene 对 reaching_defs/liveness 不 sound。EANBudget：round=30, node=200k, time=5s。
- **函数上限**：结构/计时 `--max-func-insts=300`，正确性 150；超限函数记为 skipped（论文 exclusion criterion）。
- **计时**：`--repeat=5` 取每函数中位数；聚合用几何均值；**8-worker 并行**采集（计时含并发争用，比值 config/Default 同负载下仍可比）。
- **超时**：每调用 180s（正确性 45s），超时**保留已流式输出的部分数据**（大模块贡献其完成的函数），并计入 timeouts。
- **峰值内存**：进程 `PeakWorkingSetSize`（Windows）。

## Table IV（语料，归一化前）→ `real_table4_corpus.csv`
| Family | Programs | Functions | IR insts | Roots | DAG nodes | Star nodes | Excluded(fn) | Timeouts(prog) |
|---|---|---|---|---|---|---|---|---|
| coreutils | 109 | 17,023 | 623,215 | 623,186 | 24,050,362 | 2,920 | 368 | 0 |
| open | 24 | 129,876 | 3,039,428 | 3,025,994 | 114,106,682 | 11,623 | 4,019 | 12 |
| spec | 5 | 24,676 | 817,451 | 817,093 | 34,691,072 | 6,904 | 895 | 1 |
| **Total** | **138** | **171,575** | **4,480,094** | **4,466,273** | **172,848,116** | **21,447** | **5,282** | **13** |

→ 直填 Table IV 的 family-1(coreutils)/family-2(open)/family-3(spec) 三行 + Total。

## RQ1 正确性（sound safe-minimal）→ `real_rq1_correctness.csv`
**5 个 client、约 514 万条指令级 fact 对比、`unequal = 0`、`missing = 0`**（constant_prop 有 8 处分析段错误，已逐-client 隔离，不污染其他）。

| client | laws | functions | in-lines | unequal |
|---|---|---|---|---|
| reachable | safe | 74,511 | 1,651,282 | **0** |
| reaching_defs | safe | 28,646 | 710,715 | **0** |
| liveness | safe | 43,777 | 1,055,455 | **0** |
| uninitialized | safe | 74,511 | 1,651,282 | **0** |
| constant_prop | safe | 6,353 | 160,906 | **0** |

**Answer to RQ1**：safe-minimal EAN 在真实语料上**保持全部 client 结果**（0/5.1M 不一致）。**Greedy 同样 0/5.1M 不一致**（`real_rq1_correctness.csv` 的 `variant=greedy` 行）——因其只用左分配因式分解 + 结合律规范化，无任何分配律假设，故对所有 client sound。

### 律法-参数化验证（过度声明不 sound）→ `real_rq1_kleene_divergence.csv`
用 **full Kleene** 档案（含右分配律+滑动）跑同样对比，在完整数据对上：reachable **435/987k（0.04%）**、reaching_defs **19,050/804k（2.4%）**、liveness **23,009/811k（2.8%）** 出现不一致——即这些 client **不是真正的 Kleene 代数**，右分配律/滑动对其**不 sound**；而 safe-minimal 全 0。这在真实语料上量化验证了论文的 R1（law-gated admissibility）契约。

## Table VI（最终复杂度，相对 Default 的几何均值）→ `real_table6_complexity.csv`

> **注（e-graph 优化刷新，2026-08-17）**：EAN 内部已升级(typed-DSL 节点 + 增量 rebuild + 父结点工作队列抽取器 + guarded expansion),端到端 EAN 归一化在富客户端(reaching_defs)上实测 **1.72×** 提速(`egraph_speedup.md`);**正确性重验通过**(下方 RQ1 全 0 不一致)。EAN/Order+EAN 行的节点比因 tie-break(atom 数值序 + union-by-size)微移 ≤2%;Greedy/Order 行为非-EAN 路径,数值不变。

| Config | Unique nodes | DAG edges | Tree size | Sequence | Stars | Sharing(before→after) |
|---|---|---|---|---|---|---|
| Greedy | 0.397 | 0.257 | 0.998 | 0.247 | 1.000 | 4.39→18.3 |
| Order | 0.367 | 0.226 | 1.078 | 0.220 | 0.993 | 4.39→115.2 |
| EAN | 0.395 | 0.256 | 0.999 | 0.245 | 1.000 | 4.39→18.8 |
| **Order+EAN** | **0.352** | **0.212** | 1.037 | **0.205** | 0.994 | 4.39→111.6 |

- **唯一节点降到 ~0.34–0.40×（几何均值省 60–66%）**；按总节点加权省 **84–91%**（大函数收益更大，见 RQ2 桶）。**Sequence(concat) 降到 ~0.20–0.25×** 是主贡献（因式分解压缩共享前/后缀）。
- **Greedy(0.397) ≈ EAN(0.394)** —— 两者共享同一规范化（ACI/结合律）与左分配因式分解；差别仅在 EAN 保留竞争形做 reuse-aware 跨根代价抽取，而 Greedy 做逐类最廉树抽取（`reuseIters=0`）。**在逐函数 summary 批次上，保留竞争形几乎无额外收益（<1% 节点）**——共享子表达式在单函数内已被工厂 hash-cons 捕获，跨根重用空间很小。这是一个如实的负面结果：cost-driven 抽取的价值需要更大的跨查询/跨函数批次才能显现。
- **Order+EAN(0.344) 现在同时优于 Greedy/EAN/Order** —— 靠 **单调守卫**（见下方 Part B）：EAN 逐函数取 `min(Order, EAN-on-Order)`，能叠加处叠加、不能处退回 Order，故永不更差、常更好。
- Order 的 tree size >1（1.078）：cost-aware 序在压低唯一节点/提高共享的同时可能增大展开树——印证"tree size 单独会误判表示"。

## Table VII（性能，相对 Default 的几何均值）→ `real_table7_performance.csv`
| Config | Generation | Normalization | Interpretation | End-to-end | Peak RSS | Timeouts |
|---|---|---|---|---|---|---|
| Greedy | 1.301 | 6.03 | 1.499 | 6.609 | 1.316 | 20 |
| Order | 1.352 | — | 1.051 | 1.333 | 1.118 | 13 |
| EAN | 1.312 | 6.66 | 1.549 | 7.171 | 1.334 | 22 |
| Order+EAN | 1.740 | 5.63 | 1.554 | 6.671 | 1.288 | 19 |

- **诚实结论**：这些 dataflow client 的**解释本身极廉**（µs 级），而 EAN 的饱和是 ms 级，故 **EAN end-to-end 慢 ~6.7×**、峰值 RSS 高 ~1.3×（e-graph 瞬时开销）。即在此语料上 **EAN 是"IR 体积/保留表示"优化，而非速度或峰值内存优化**。
- **计时口径**：此表用 `reachable`(退化 1-fact 客户端),其 DAG 结构简单、抽取占比小,故 e-graph 抽取器提速在此**杠杆有限**;富客户端(reaching_defs)上归一化实测 **1.72×**(`egraph_speedup.md`)。计时有 run-to-run 抖动。
- Order 生成慢 1.35×（min-product 排序 + 略增 fill），但无 EAN 后处理。
- **Greedy 与 EAN 计时同量级**：同一 e-graph 饱和主导开销,`reuseIters=0` 省下的仅是抽取阶段的极小部分。即 Greedy 相对 EAN **不是更快的近似**,而是**同等开销、同等 IR、缺少 reuse-aware 抽取**的消融点。

## RQ2 Break-even → `real_rq2_breakeven.csv`
按 raw DAG 节点数分桶的 (Default vs EAN) end-to-end 中位数比：
| raw nodes | n | Default µs | EAN µs | EAN/Default |
|---|---|---|---|---|
| 0–50 | 41,442 | 32 | 208 | 6.5× |
| 100–200 | 6,295 | 178 | 1,274 | 7.2× |
| 1k–5k | 7,693 | 2,130 | 20,467 | 9.6× |
| >5k | 3,420 | 13,666 | 164,880 | 12.1× |

→ **在所有规模桶上 EAN 都更慢，且随规模单调加重**（5.9×→9.8×）。故本语料上 EAN 不存在时间盈亏平衡点：其收益在 IR 体积（Table VI）而非端到端时间。这为论文"invocation gate / 仅对大表达式启用"的必要性提供了直接证据。

## RQ3 Peak（构造节点，Order vs Default）→ `real_rq3_peak.csv`
| Family | functions | Default peak(mean) | Order peak(mean) | Order/Default(geomean) |
|---|---|---|---|---|
| coreutils | 16,299 | 1038.8 | 1316.5 | 1.078 |
| open | 129,300 | 512.7 | 647.0 | 1.035 |
| spec | 17,773 | 1134.0 | 1441.6 | 1.055 |

→ **诚实结论**：真实 CFG 多为可归约，Default 的逆拓扑序已接近最优，cost-aware min-product 在真实语料上**略微升高峰值（1.03–1.08×）**——与合成语料（环状/不规则图上 Order 显著降峰）互补：**Order 的价值在真实语料上体现为最终节点（0.367×）而非峰值**。

## 尚未覆盖 / 后续
- **Order+EAN 叠加性 / EAN 时间开销**：可用 reuse-aware DAG 抽取代价 + invocation gate 改善（RQ4 方向）。
- spec 巨模块（dealII 30MB 等）多为部分数据（超时保留），已如实计入 timeouts/exclusions。

## 文件清单（真实语料）
| 文件 | 论文位置 |
|---|---|
| `real_table4_corpus.csv` | Table IV（coreutils/open/spec 三行 + Total）|
| `real_table6_complexity.csv` | Table VI（Order/EAN/Order+EAN，safe-minimal）|
| `real_table7_performance.csv` | Table VII（generation/norm/interp/end2end/RSS/timeouts）|
| `real_rq1_correctness.csv` | RQ1 正确性（0/5.1M）|
| `real_rq1_kleene_divergence.csv` | RQ1 律法-参数化验证（kleene 不 sound 的偏差）|
| `real_rq2_breakeven.csv` | RQ2 break-even 分桶 |
| `real_rq3_peak.csv` | RQ3 peak 构造节点（Order vs Default）|
| `real_eval.py` | 复现驱动（`run` / `agg`）|

---

# RQ2 补充：Invocation gate + 摊销 break-even（真实语料）

数据：`real_rq2_gate.csv`、`real_rq2_amortized.csv`（`real_eval.py` 的 amort passes，`--interp-repeat=50` 取稳定 per-query 解释时间）。EAN=safe-minimal。

## Invocation gate 扫描 → `real_rq2_gate.csv`
`--ean-min-nodes=T`：原始唯一节点 < T 的批跳过 EAN（走 I3 原样返回）。85,400 个函数上：

| gate T | 跑 EAN 的函数 | gated end-to-end / Default |
|---|---|---|
| 0（全跑）| 85,400 | 6.20× |
| 100 | 30,636 | 6.12× |
| 1000 | 11,705 | 5.82× |
| 5000 | 3,579 | 4.85× |

**诚实结论**：gate 把绝大多数小函数排除出 EAN，但**总端到端仅从 6.20× 降到 4.85×**——因为总时间由少数超大函数主导，而它们恰在阈值之上仍会跑 EAN。故 gate 是"避免在不值得的小表达式上浪费"，而非总时间的主要杠杆；其真正价值在下面的摊销视角。

## 摊销 break-even → `real_rq2_amortized.csv`
现解释器是**值折叠、无 memo**，成本 ∝ 展开树。用 `--interp-repeat=50` 取稳定 per-query 解释时间后（全语料求和，µs）：

| 模型 | gen(def) | norm(ean) | interp/query(def) | interp/query(ean) | per-query 加速 | **break-even K\*** |
|---|---|---|---|---|---|---|
| **non-memoizing（实测）** | 122.7M | 1132.5M | 55.9M | 51.7M | **1.08×** | **269** |
| **memoizing（投影，∝唯一节点）** | 122.7M | 1132.5M | 1.51M | 0.20M | **7.58×** | **864** |

**核心结论（RQ2 正向、可填论文）**：
- 之前 Table VII 报"EAN 解释慢 1.36×"是**单次 µs 级测量噪声**；50 次稳定测量下 **EAN 每次解释实际快 1.08×**（更廉 IR）。
- EAN 的归一化是一次性大开销（≈9× 总 generation）；**当每个 summary 被解释 ≥ 269 次时，EAN 累计端到端超过 Default 转正**——正对应论文"summary root 会被 client 反复解释"的前提。
- **memoizing 解释器下 per-query 快 7.58×**（EAN 节点缩减直接兑现），但 break-even K\* 更大（864）——因为 memo 让解释绝对成本本就很低，固定的归一化开销需要更多次查询摊销。这量化了论文 C_repeat/γ 项区分的"memo vs 非memo"权衡。

**Answer to RQ2**：单次解释下 EAN 端到端更慢（IR 体积优化，非速度）；但 EAN 的更廉 IR 在**反复解释**下摊销——非 memo 解释器 **269 次**、memo 投影 **864 次**查询后净赚。gate 可将小表达式排除以减少无谓开销。

## 新增文件
| 文件 | 论文位置 |
|---|---|
| `real_rq2_gate.csv` | RQ2 invocation-gate 扫描 |
| `real_rq2_amortized.csv` | RQ2 摊销 break-even（memo vs 非memo）|

---

# Part B：Order+EAN 叠加（RQ4 抽取器 + 单调守卫）

## 现象与根因
初版真实语料上 **Order+EAN(0.381) 未优于 Order(0.367)**。逐代码定位（`BatchExtract.dagCost`/`Export`）：导出工厂节点数 ≈ **eUnique（变长孩子边，重新二叉化后）**，而 `uniform` 代价（β=0）只按 **nUnique（e-类数）** 打分 → 抽取选"类少但边多"的形。

## RQ4 抽取器对比（uniform vs reuse-aware DAG cost）
加 `CostModel::dag()`（β=1 计边）+ CLI `--ean-cost=dag`，spot-check（sum dagstats nodes）：

| 程序 | Order | Order+EAN(uniform) | Order+EAN(dag) |
|---|---|---|---|
| b2sum | 465,414 | 482,980 | 481,666 |
| make-prime-list | 79,631 | 87,482 | 87,591 |
| cat | 204,965 | 210,385 | 210,199 |

**RQ4 结论（如实）**：真实语料上 **DAG 代价 ≈ uniform（差 ~0.3%），未能让 Order+EAN 叠加**。根因不是代价，而是**根本性不组合**：EAN 的规范因式形对这批 summary 有"固定尺寸"，而 Order 的按消除序因式分解**已更紧凑**（Default 620k→EAN 532k 是降；Order 465k→EAN 482k 是升）。EAN 安全网只保证"≤ 自身规范图树抽取"，不保证"≤ 输入"。

## 单调守卫（真正修法）
`ExtractOptions.monotoneGuard`（CLI `--ean-monotone`）：`ean()` 导出后若唯一节点数 > 输入则**原样返回输入**——把 I3（保根）扩展为**保质量**，EAN 永不退化。逐函数即 `min(Order, EAN-on-Order)`。

**结果（`real_table6_complexity.csv`，守卫开启）**：**Order+EAN 唯一节点 0.381→0.344**，**同时优于 Order(0.367) 与 EAN(0.394)**；DAG edges 0.207、sequence 0.200 均为最佳。spot-check：make-prime-list 87482→79630（=Order）、cat 210385→**204090（< Order 204965，真叠加）**。RQ1 仍 **0 不一致**（守卫只改选哪个等价形，语义不变）。

**Answer（Part B）**：抽取代价（uniform/DAG）在真实语料上对 Order+EAN 叠加无实质帮助；真正机制是**单调守卫**——它保证 Order+EAN ≥ 两者中更优、常严格更优（0.344 < 0.367），恢复了论文预期的"构造次序与表示优化互补叠加"。

## 新增/改动
- `CostModel::dag()`；`ExtractOptions.{gateMinNodes 已有, monotoneGuard}`；`EliminationOptions.{EANMinNodes, InterpRepeat, EANMonotone}`。
- 工具 CLI：`--ean-cost=uniform|dag`、`--ean-monotone`（+ 前述 `--ean-min-nodes`/`--interp-repeat`）。
- 默认全 opt-in（关）→ native apa_tests 137/138 无回归。

---

# 过程间 EAN（M6b）— 接线、soundness、单调守卫必要性与可扩展性墙

EAN/Greedy 已接入过程间 **path-summary 求解器** `ForwardInterSummarySolver`（把整个程序建成一张全局 path-summary 方程图，每个 `(指令,调用串上下文)` 求出闭式正则路径表达式再解释）。**EAN 引擎零改动**即复用：`ean<TransferT>` 对 transfer 类型完全泛型（原子按 `Expr*` 指针去重、对 EAN 不透明、导出原样搬回），故 `ean<atom_t>` / `greedySimplify<atom_t>` 直接跑在 summary 批上（`atom_t = InterSummaryTransferAtom`）。

## Level 1（接线 + soundness）— 已完成并验证
- 改动全 opt-in（默认 no-op → 零行为变化）：`InterEANOptions`（Options.h）；`InterSummarySolveDiagnostics` += gen/norm/interp 计时 + 优化前后完整 `DagStats`；`PathSummaryEquationOptions.EAN` 字段 + `PathSummaryEquationResult` 非 const `summaries()`；`ForwardInterSummarySolver::applySummaryPostPass`（求解与解释之间批量 EAN/Greedy）。
- **soundness**：inter 解释器 Concat=顺序 apply、Union=merge（同 intra），且此处每个原子 `apply` 是 `In` 纯函数 → 左分配律 `(a·b)⊕(a·c)=a·(b⊕c)` **无条件成立** → 默认 safe-minimal 保全部 inter client 结果。
- 测试 `EAN/InterWiringTest.cpp` **6/6 通过**：EAN/Greedy 与 Default 逐 `(inst,ctx)` fact 相等；`(a·b)⊕(a·c)` 在 atom_t 上确定性缩减 6→5；**300 组随机 rooted DAG 差分 EAN==Default**。native apa_tests 147/148（仅既有 EliminationTest 失败，无回归）。
- 工具 `--inter-summary` 端到端可用,发 `[inter-summary]`/`[dagstats-before]`/`[dagstats-after]`/`[profile]` 行。

## KEY 发现 1：inter 侧单调守卫是必需的（真实数据点：998.specrand）
在唯一能完成的真实程序 998.specrand（6.5K，111 contexts，raw batch 983 unique nodes）上，inter_reachable summary：

| 配置 | unique nodes | edges | seq | 说明 |
|---|---|---|---|---|
| Default (= before) | 983 | 1710 | 625 | 原始 summary 批 |
| EAN **无守卫** | **1220** | 2184 | 873 | **膨胀**（tree 略降 172.6k→169.2k）|
| EAN **有守卫** | 983 | 1710 | 625 | 守卫退回输入 |

→ **无守卫 EAN 会膨胀 inter summary**：path-summary 求解器的 Floyd–Warshall 式 SCC 闭包构造**已高度因式化**，EAN 的重二叉化规范形反而更大。**单调守卫（`--ean-monotone`）在 inter 侧是必需的**——这是 intra Part B 发现的强化版（inter 的 summary 构造比 intra 逐函数更"已优化"，故 EAN 更难净胜）。时间：gen 8.6ms / norm(EAN) 57ms / interp 2.2ms。

## KEY 发现 2：path-summary 求解器不 scale（可扩展性墙，K 无关，非 EAN）
真实语料上此求解器基本不可用——**generation（方程图求解）阶段爆炸**，与 EAN 无关：

| 程序 | 大小 | 结果 |
|---|---|---|
| 998.specrand | 6.5K | ✅ 完成（111 contexts, 13ms） |
| 429.mcf（**无 EAN** default） | 94K | ⏱ 挂起 >90s |
| 470.lbm / 401.bzip2 | 94K / 467K | ⏱ 超时 |
| 462.libquantum | 203K | 💥 SIGILL |
| 458.sjeng | 743K | 💥 SIGSEGV |
| bc14 coreutils ×9（base32/cat/cut/…，135–180K） | — | ⏱ **0/9 完成**（全超时）|

**根因**：`PathSummaryEquationSolver` 的 cyclic-SCC 矩阵闭包（`M[i][j]|=M[i][k]·M[k][k]*·M[k][j]`）在程序级方程图上产生**天文级正则表达式**（O(N³) 结构爆炸）。**与调用串 K 无关**：K=2/1/0 在 mcf/libquantum/cat 上**全部超时**（实测），证明瓶颈不是上下文敏感度而是 path-summary 表达式尺寸。mcf 无 EAN 也挂 → 非 EAN 问题。

**结论（如实）**：过程间 EAN 的**接线正确、soundness 已证**，但**真实语料 Table VI/VII/RQ1 无法通过此 path-summary 求解器产出**（几乎无程序完成）。这是预存求解器的成熟度限制，是一个诚实的负面结果。inter 侧 EAN 的可评测化需要一个能 scale 的过程间构造（如带 summary 复用/记忆化、或表达式尺寸上限的 anytime 变体），属未来工作。

## 新增/改动（M6b）
- `InterEANOptions`（Core/Options.h）；`InterSummarySolveDiagnostics` += `gen/norm/interp_time_us` + `ean::DagStats summary_before/after`（InterResult.h include DagStats.h）。
- `PathSummaryEquationOptions.EAN`；`PathSummaryEquationResult` 非 const `summaries()`；`ForwardInterSummarySolver`（gen 计时 + `applySummaryPostPass` + 诊断透传）。
- 工具 `--inter-summary` + `runInterSummary*`(reachable 干净;reaching_defs/uninit/constant_prop 传 nullptr 附加分析) + `[inter-summary]`/`[dagstats-*]` 输出。
- 测试 `EAN/InterWiringTest.cpp`（6）。默认全 opt-in → native apa_tests 147/148 无回归。

---

# 仿射关系分析作为 EAN 客户端（Affine，intra）— Stage 1 + 共享域 bug 修复

新建一个**过程内仿射等式分析**跑在 path-expr `IntraEliminationSolver` 上(intra EAN 工作且在 bc14 上 scale 的路径),使 affine 成为论文 **R1 的正面案例**:一个**分配式**客户端,**full-Kleene EAN 在此 sound**(对比 reachable/liveness 上 kleene 不 sound)。

## 现状勘察结论
affine **原本不在 EAN 路径上**:`AffineRelationDomain` 仅被 `InterAffineEqualities` 经上下文敏感的 `InterEliminationSolver` 使用(不构造 path-expr 批)。故需新建走 path-expr 求解器的 intra affine 分析。

## 契约绑定(核心)
`ElimAffineProblem : LLVMIntraEliminationProblem<AffineFact>` 只覆写代数;节点=指令,transfer_t=Instruction*:

| 契约 | 绑定 | 语义 |
|---|---|---|
| `meet`(Union+Star 合并) | `D::combine` | join / 仿射包(**非** `D::meet`=交) |
| `applyTransfer(T,In)` | `D::extend(instructionTransfer(T),In)` | 关系复合 |
| `meetIdentity()` | `D::zero()` | ⊥ |
| `initialFact()` | `D::identity()` | 对角关系 |
| `equal_to` | `D::equal` | Howell 范式规范相等 |

- 指令→transformer(最小自包含):add/sub/mul(常量)→`makeAffineAssignment`(仅直接操作数),phi 与其余定义 tracked 标量→`makeForget`(sound havoc),非 tracked→`identity()`。
- vocabulary = **单一主导位宽**(混位宽会让域近似到 64/失准);`configure(&vocab)` 静态单例,同步求解期间存活。
- **EAN 零改动即用**(fact=AffineRelation、transfer 对 EAN 不透明)。

## KEY 发现:修复共享域的一个预存正确性 bug
借由 affine-EAN 工作**发现并修复** `AffineRelationDomain` 的一个微妙 bug(inter affine 客户端也依赖此域,故一并受益):
- `normalizeComponent` 的矛盾检测用**固定列号 `2*vars`** 当增广(常数)列;但 `composeComponent`(3*vars+1 宽)/`joinComponent`(4*vars+2 宽)会在**更宽的中间矩阵**上调用它,此时列 `2*vars` 是**中间变量列而非常数列**。
- 后果:一个可满足的非齐次关系,当其偏移**恰为单位元 1**(如 `v'=u+1`,即 `i++`,真实代码无处不在)时被误读为 `1=0` → **假 bottom**(实测仅常数=1 触发;0/2/3/4… 均正常)。
- 修法:检测锚定在**行自身的最后一列**(`leadingIndex(row)==row.size()-1 && row.back().isOne()`),在任意矩阵宽度下都正确。
- 影响:此前 intra affine 在真实代码上会产出全 bottom(不可用);修复后产出有信息的仿射事实。**这是一个真实的域正确性修复,inter affine 精度亦受益。**

## Stage 1 验证(全部 native,已注册)
`EAN/AffineEanTest.cpp`:
- **TransferDistributesOverJoin**:`extend(T,combine(a,b))==combine(extend(T,a),extend(T,b))` **非平凡**成立(钉住 kleene-sound 前提)。
- **ComputesNonTrivialAffineFacts**:断言存在有信息(非 bottom、非 identity)事实 → 使下列保持性测试**非空**(修 bug 前此测试暴露了全 bottom)。
- **EanSafePreservesFacts / EanKleenePreservesFacts / GreedyPreservesFacts**:Default vs EAN(safe)/EAN(kleene)/Greedy 逐指令 IN fact `equal` → **affine 上 kleene sound(非空验证)**,核心正面 R1 结果。
- DomainComposeThreeVars:上述域 bug 的回归测试(常数 0..16 复合均不 bottom)。

**结果**:64 个 affine 相关测试全过(AffineEan 6/6 + AffineRelationDomain 24 含此前被我误修一度失败的 GuardedKSToMOSVariants + InterAffineEqualities),full native apa_tests **153/154**(仅既有 EliminationTest 失败,无回归)。

## 新增/改动（Affine Stage 1）
- 新增 `Analyses/Intra/IntraAffineEqualities.{h,cpp}`(注册进 APADataFlow)。
- 修复 `Domains/AffineRelationDomain.cpp::normalizeComponent` 矛盾检测列号(共享域正确性修复)。
- 新增测试 `EAN/AffineEanTest.cpp`(注册进 apa_tests)。

## Stage 2:工具客户端 + 可扩展性发现(语料不可行)
**工具 affine intra 客户端已建成并端到端验证**(`--analysis=affine`,含无 vocab 的规范 fact 序列化器):
- 小函数实测(`t(i32 %n)`:赋值+分支+phi,9 roots):`[dagstats]` **default 34 → EAN-safe 23 → EAN-kleene 21** 唯一节点 —— EAN 降 affine path-expr DAG,且 **kleene 比 safe 降更多**(右分配+滑动可用);`interp_us≈2661`(远高于 reachable 的 µs 级,affine 解释本身昂贵)。
- **default / ean-safe / ean-kleene 三配置逐指令 fact 字节级相同** → **affine 上 kleene sound 在工具层再次确认**。

**可扩展性墙(如实,语料不可行)**:affine intra **eval** 在真实 bc14 函数上超时——即使 `--max-func-insts=100` 也 60s 超时。根因:affine 域 `extend`/`combine` 是 O(vars³) 的 Howell 模运算,叠加**非记忆化树解释器**(eval 成本 ∝ 展开树大小)+ 循环的 Star 不动点迭代,在多变量真实函数上爆炸。故 **affine 真实语料 Table VII/RQ1 无法产出**。

**两点结构性认识**:
1. **Table VI 与域无关**:path-expr 结构是纯 CFG 路径代数,affine 的与 reachable 的**相同**。affine 特有的价值只在**律法档案**:kleene 给出额外降幅(21 vs 23)**且在此 sound**(reachable 上 kleene 不 sound)。
2. affine 是一个**解释昂贵**的客户端(µs→ms),这为论文 RQ2"EAN 的收益是摊销的 IR 体积、存在 break-even"提供了对照方向(reachable 解释太廉,affine 解释昂贵)——但 affine eval 太慢无法在语料上量化。

**结论**:affine 作为 EAN 客户端的**接线正确、kleene-soundness 已证(单元+工具)、EAN 降 IR 已见**;真实语料评测受限于 affine 域自身的求解开销(非 EAN 问题),列为未来工作(需记忆化解释器或有界 affine 求解)。

## 新增/改动（Affine Stage 2）
- 工具 `lotus-dfa-apa`:新增 `affine` intra 客户端 + 无 vocab 规范 fact 序列化器 `serializeAffine`;`--analysis` 帮助与 `Handlers[]` 更新。默认全 opt-in。

---

# 记忆化解释器（Memoizing Interpreter）— 量化论文 C_repeat/γ 项

**动机**:当前树遍历解释器 `eval(E,In)` 对多父复用的 DAG 节点重复求值 → 成本 ∝ **展开树大小**(论文 Eq.5 的 `C_repeat` 项的来源)。对**解释昂贵**的客户端(affine),这直接导致超时。

**记忆化 transformer 求值(affine 专属,opt-in `--memo-interp` / `EliminationOptions.InterpMemo`)**:给每个唯一 DAG 节点算一次它的 transformer(按 `Expr*` 记忆),自底向上 `combine`/`extend`/闭包组合;因 `initialFact=identity` 是复合幺元,`IN(node)=T(ExprTo(node))`。成本 ∝ **唯一节点数**。语义与树求值**完全等价**(结合律+幺元;单元测试 `MemoInterpreterMatchesTreeInterpreter` + 工具逐指令 fact 字节相同证明)。

**量化结果(受控 diamond-chain,tree ≫ unique)**:

| 规模 | 唯一节点 | 展开树 | 树解释 interp | 记忆解释 interp | 加速 |
|---|---|---|---|---|---|
| N=8 diamonds | 2,364 | 63,049 | **115.2 s** | **5.7 s** | **~20×**,fact 相同 |
| N=13 diamonds | 6,114 | 2,062,161 | **>120 s 超时** | 54.3 s | 树不可行 → memo 可行 |

→ **记忆化把解释成本从 ∝展开树 降到 ∝唯一节点**(N=8 加速比 ~20× ≈ tree/unique 27×);对树解释器**无法完成**的高共享用例(N=13),memo 可完成。这在真实测量上**量化了论文的 C_repeat/γ 项**(memo vs non-memo),并意味着 **EAN 降唯一节点 → 在 memo 解释器下直接降解释时间**(与非 memo 单次解释无收益形成对比,支撑 RQ2 摊销故事的正面方向)。

**局限(如实)**:记忆化去掉了树展开因子,但**未去掉 affine 单次运算的 O(vars³) 成本**。真实大函数(如 cat.bc 的 main,数百变量 + 循环闭包)即使 memo 仍超时——affine 域本身的每运算成本 + 闭包迭代是另一重墙。故 memo **解锁了高共享/中等变量数的函数**,但数百变量的大函数仍受限于 affine 域固有开销(未来工作:更廉的关系表示 / 有界 affine)。

**改动**:`EliminationOptions.InterpMemo`(默认关);`StateEliminationSolver`/`SolverContext.applyEAN/applyGreedy` 树求值守卫加 `&& !InterpMemo`(memo 时构建/优化 ExprTo 但跳过树求值);`IntraAffineEqualities.cpp` 新增 `memoInterpret`(按 `Expr*` 记忆的 transformer 求值 + 闭包);工具 `--memo-interp`;测试 `AffineEan.MemoInterpreterMatchesTreeInterpreter`。native apa_tests 154/155(仅既有 EliminationTest 失败,无回归)。

## Stage B:affine 真实语料结果(bc14,memo,`docs/eval/affine_eval.py`)
memo 解锁后,在 bc14 全 138 程序上以 `--memo-interp --max-func-insts=200`、每程序 40s 超时+部分捕获,跑三配置 {Default / EAN-safe / EAN-kleene}(全 memo)。**共完成 9,752 个函数**(coreutils 1,852 / open 7,499 / spec 401),**0 崩溃**;137/138 程序在末尾某大函数上超时,但部分捕获每程序收获数十~数百函数。

**RQ1 — affine 上 kleene sound(真实语料,R1 正面案例)→ `affine_rq1.csv`**:

| variant | 函数数 | fact 行 | **unequal** | 程序 | 崩溃 |
|---|---|---|---|---|---|
| EAN safe | 9,635 | 176,003 | **0** | 135 | 0 |
| **EAN kleene** | 9,635 | 176,003 | **0** | 135 | 0 |

→ **full-Kleene EAN 在真实语料上保持全部 affine 事实(0/176k)**,是论文 R1 的**正面案例**,与 reachable/liveness 的 kleene 偏差(2.4–2.8%)直接对照:**律法可采纳性取决于客户端代数**(affine 分配式→kleene sound;非分配式→不 sound)。

**Table VII / RQ2 — memo 下 EAN 端到端获益 → `affine_table7.csv` / `affine_breakeven.csv`**(9,752 函数):

| 指标(EAN/Default 几何均值) | 值 |
|---|---|
| 唯一节点 | **0.448** |
| **memo 解释时间** | **0.456** |

- **memo 解释时间比(0.456)紧贴唯一节点比(0.448)** → 实证记忆化使解释 ∝ 唯一节点,故 **EAN 降节点 ⇒ 直接降解释时间**。
- **break-even K\*=0**:总 norm 43.4s vs 总解释节省 **2,129s** → **EAN 第一次解释即净收益**。

→ **决定性正面 RQ2 结果**:**解释昂贵**客户端(affine)+ 记忆化下,**EAN 立即端到端加速(解释减半、norm 可忽略)** —— 与 reachable(解释 µs 级、EAN 端到端慢 6.5×)完全相反。补全论文 RQ2 两面:**EAN 的 IR 缩减仅在解释昂贵且记忆化时兑现为速度**;C_repeat/γ 是这一切换的关键。覆盖率(`affine_coverage.csv`):138 程序 / 9,752 完成 / 137 超时 / 0 崩溃(大函数受 affine 域固有开销限制,非 EAN)。

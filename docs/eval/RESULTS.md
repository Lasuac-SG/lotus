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

### 4. Table VIII（消融，相对 full EAN 的 final nodes）→ `table8_ablation.csv`

| Variant | Final nodes (vs full EAN) | 说明 |
|---|---|---|
| **No factorization** | **1.163**（多 16%）| 关掉左右分配律——**节点缩减的主导机制** |
| No star rules | 1.00 | 滑动只重结合，不改节点数（其价值在解释时间/RQ2）|
| No guarded expansion | **N/A** | Explore 阶段已推后，机制未落地 |
| No phase schedule | **N/A** | 目前单一固定调度，无"无调度"对照模式 |
| Uniform tree cost | 1.00 | 代价权重只影响等价形选择，不改本族节点数 |
| Profiled tree cost | 1.00 | 同上（区别体现在解释时间/RQ2）|
| Reuse-aware / Profiled-tree | 1.00 | 安全网取等号：本族哈希共享已被树抽取捕获 |

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

# TranslAPA_Baseline 设计方案（初稿）

> 目标：把 OOPSLA 2026 论文《Mechanically Translating Iterative Dataflow
> Analysis to Algebraic Program Analysis》(Zhou, Wang, Wang) 的“IDA→APA 机械翻译”
> 算法，作为**外部强基线** `TranslAPA_Baseline` 复刻进 Lotus/APA，用于在 Reachable
> 等分配式客户端上，与现有图优化算法（EAN 等式饱和 DAG 优化）做 **DAG 节点数** 与
> **求值时间** 的对比。
>
> 关键立场：论文自己的实现策略是“**保留计算路径表达式的前端（Tarjan），只替换解释路径
> 表达式的后端**”。这与 Lotus 现状天然对齐——我们复用消元前端产出的路径表达式 DAG
> `Results.ExprTo(N)`，只**换解释器**。因此本基线不是一条平行管线，而是现有管线上的一个
> 新“语义函数/解释器”。

---

## 1. 论文核心映射规则总结

### 1.1 两套框架的对象对照

| | IDA（迭代数据流） | APA（代数程序分析） |
|---|---|---|
| 数据流空间 | `Λ = 2^D`（事实集 `D` 的幂集） | 程序性质空间 `𝒟` |
| 传递函数 | 抽象变换器 `⟦e⟧ : Λ→Λ`（仅对边 `e∈E` 定义） | 语义函数 `D⟦·⟧ : 𝒟`（对边**和任意路径表达式**定义） |
| 求解方式 | 对所有边反复施加 `⟦e⟧` 到不动点 | 先算路径表达式 `p`（正则式），再用 `⊗ ⊕ ⊛` **代数地**解释 `p` |

论文的机械翻译 = 给出 **`Λ ↦ 𝒟`** 与 **`⟦e⟧ ↦ D⟦e⟧`** 两个映射，并补齐 IDA 中不存在、
APA 必需的三条合成律 `D⟦p₁p₂⟧ / D⟦p₁+p₂⟧ / D⟦p₁*⟧`。翻译“机械”指：**不依赖事实的语义**，
只依赖 `Λ` 是某个有限集 `D` 的幂集、且 `⟦e⟧` 分配于 `∪`。

适用条件（IFDS 类）：(1) `D` 有限；(2) `⟦e⟧` 对 confluence（如 `∪`）分配。

### 1.2 Gen/Kill 类（§4，separable，Kildall 框架）

**空间翻译**：`𝒟 = (Gen, Kill)`，其中 `Gen, Kill ⊆ D`（同一个 `D` 的两个子集）。
IDA 的 `Λ = 2^D` ⟶ APA 的 `𝒟 = 2^D × 2^D`。

**边原子**：由语句直接给出的 `(Gen_e, Kill_e)`；语义为
`⟦e⟧(x) = Gen_e ∪ (x \ Kill_e)`。

**三条合成律**（`fᵢ(x)=Genᵢ∪(x\Killᵢ)`）：

- 顺序 `p₁p₂`（先 `p₁` 后 `p₂`，即 `f₂∘f₁`）：
  - `Gen₁₂ = Gen₂ ∪ (Gen₁ \ Kill₂)`
  - `Kill₁₂ = Kill₁ ∪ Kill₂`
- 选择 `p₁+p₂`（`f₁ ⊔ f₂`，分支合流）：
  - `Gen₁₊₂ = Gen₁ ∪ Gen₂`
  - `Kill₁₊₂ = Kill₁ ∩ Kill₂`
- 星 `p₁*`（`ε + p₁ + p₁p₁ + …`，用幂等 `f₁f₁=f₁`）：
  - `Gen₁* = Gen₁`，`Kill₁* = ∅`

**单位/零元**（工厂已结构化短路，解释器仍应定义）：
`one(ε)=(∅,∅)`；`zero=(∅,D)`（`+` 的单位：`Kill=D`，被 `∩` 吸收）。

**复杂度**：`O(|E|·|D|)`（集合运算线性于 `|D|`），与原 IDA 同阶。

### 1.3 非-Gen/Kill 类（§5，IFDS，distributive 非 separable）

动机：`⟦e⟧(x)` 需要**依赖输入**、含多分支（如 `z:=x+y` 时“x 或 y 未初始化 ⇒ z 未初始化”）。
直接展开分支会指数爆炸（最坏 `2^|D|`），故**位向量 + 布尔矩阵**表示。

**空间翻译**：`𝒟 = (M, C)`，`M` 是 `|D|×|D|` 布尔矩阵，`C` 是长度 `|D|` 的常量位向量。
语义：`⟦e⟧` 的位向量形式 `S' = M·S + C`（`·`=布尔矩阵乘，`+`=按位或）。可视为“扩展的
Gen/Kill”：`M·S` 依据流关系“杀”，`C` 无条件“生”。

**边原子**：例如 `x:=y` ⇒ `M[x][y]=1, C=0`；`x:=42` ⇒ `M[x][*]=0, C=0`；对角线初始为 `I`
（每个事实依赖自身），赋值语句改写对应行。

**三条合成律**（`S:=M₁S+C₁`, `S:=M₂S+C₂`）：

- 顺序 `p₁p₂`：`M₁₂ = M₂·M₁`，`C₁₂ = M₂·C₁ + C₂`
- 选择 `p₁+p₂`：`M₁₊₂ = M₁ | M₂`，`C₁₊₂ = C₁ | C₂`
- 星 `p₁*`：`M₁* = (I | M₁ | M₁² | …)`（传递闭包），`C₁* = M₁*·C₁`

**复杂度**：一般 `O(|E|·|D|³)`（布尔矩阵乘 / Warshall 传递闭包）；稀疏时降到
`O(σ·|E|·|D|²)` 或 `O(σ·|E|·|D|)`，`σ` 为稀疏度（实测常 ≈2）。

### 1.4 代数性质与“为什么快”（§6）

- 两个子类都构成 **幂等半环**（idempotent semiring）；流不敏感问题为**交换**幂等半环
  ⇒ 可任意顺序 divide-and-conquer 组合。
- **关键：星是闭式的**（Gen/Kill 星 = `(Gen₁,∅)` O(1)；IFDS 星 = 一次传递闭包），
  **无外层不动点迭代**。这正是与 Lotus 现有通用解释器（对 `Star` 迭代到稳定）最本质的区别。
- 合成性 ⇒ 增量分析友好：`p₁` 改成 `p₁'` 时 `D⟦p₁'p₂⟧` 只需重算 `p₁'` 并复用 `D⟦p₂⟧`。
- 论文实测（DaCapo 13 个基准）：翻译版 APA 在非增量上与手工 APA 相当或更快；增量上
  Gen/Kill 平均 21×、非-Gen/Kill 平均 2× 于手工 APA。**它是一个很强的基线。**

---

## 2. 与 Lotus 现有求解器的对接分析

已核对的关键文件与事实：

- **路径表达式 DAG**：`include/Dataflow/APA/Core/PathExpr.h`
  `PathExprFactory<TransferT>`，节点 `Zero/One/Atom/Union/Concat/Star`，hash-consed（`unite/
  concat/star` 去重并结构化短路 zero/one）。**这就是论文的路径表达式 `p`**，`TransferT` 对
  LLVM 客户端 = `llvm::Instruction*`（不透明原子）。
- **问题接口**：`Core/Problem.h` `IntraEliminationProblem`：`applyTransfer(T,In)`、`meet`、
  `meetIdentity`、`initialFact`、`equal_to`、`maxStarIterations`。LLVM 适配器
  `Adapters/LLVM/ForwardProblem.h`：`transfer_t=Instruction*`，`edgeTransfer(Src,_)=Src`。
- **现有解释器**：`Solver/SolverContext.h::eval(E,In)`——通用**树遍历不动点**解释器：
  `Atom→applyTransfer`、`Union→meet`、`Concat→顺序 apply`、`Star→meet 迭代到 equal_to 稳定`
  （受 `MaxStarIterations` 限制）。这是当前 APA 的“语义函数”，**逐原子重跑具体 transfer 且星要迭代**。
- **前端**：`Solver/{StateEliminationSolver,ADTSimpleSolver,ADTDelayedSolver}.h` 产出每节点
  `Results.ExprTo(N)`（= `p`）。
- **EAN 优化器**：`EAN/EAN.h ean(...)` 对 `ExprTo` 批做等式饱和 DAG 重写→更省节点的等价 DAG，
  再用同一 `eval` 重解释（`SolverContext::applyEAN`）。
- **对比指标已存在**：`EAN/DagStats.h computeDagStats`（`uniqueNodes/edges/tree/sharing/各类计数`）；
  `Core/Options.h SolveDiagnostics`（`gen_time_us/norm_time_us/interp_time_us`、`peak_matrix_nodes`）。
  CLI `tools/dataflow/lotus-dfa-apa.cpp` 已打印 `[dagstats]` 与 `[timing]`（见 540–562、735–756 行）。

**结论（对接点）**：三种“语义函数”共享同一 `p = ExprTo(N)`，可做严格苹果对苹果对比：

1. `eval(p)`——Lotus 现状通用解释器（星迭代）；
2. `eval(EAN(p))`——我们的图优化（更少 DAG 节点→更快 `eval`）；
3. **`TranslApaFold(p)`——论文闭式半环折叠（本基线，星闭式、记忆化单遍）。**

---

## 3. 设计方案（适配现有求解器）

### 3.1 模块划分（新目录 `Dataflow/APA/Baseline/TranslAPA/`，尽量 header-only 模板）

```
include/Dataflow/APA/Baseline/TranslAPA/
  Semiring.h          // Semiring 概念（约定接口）与 One/Zero/seq/join/star/apply
  GenKillSemiring.h   // §4：element = (Gen, Kill)，用 llvm::BitVector
  IFDSMatrixSemiring.h// §5：element = (M, C)，位打包布尔矩阵 + 传递闭包(Warshall)
  AtomTranslator.h    // 机械抽取：探测 client 的 applyTransfer → 每原子的半环元素（带缓存）
  FoldInterpreter.h   // 对 PathExprFactory DAG 的记忆化自底向上折叠（每唯一节点算一次）
  TranslApaStats.h    // 半环侧规模指标（矩阵 nnz / 位向量 popcount / 折叠耗时）
  Driver.h            // runTranslApaBaseline(...)：复用前端拿 ExprTo，折叠，产出 facts+stats
lib/Dataflow/APA/Baseline/TranslAPA/   // 非模板胶水与显式实例化（如需）
```

### 3.2 `Semiring` 概念（编译期 duck-typing）

```cpp
// element type E; 无副作用、可拷贝、可比较相等（差分/去重用）
struct SemiringConcept {
  using Elem = ...;
  Elem zero() const;                       // + 的单位（annihilator of ·）
  Elem one() const;                        // · 的单位（ε）
  Elem seq (const Elem&, const Elem&) const;   // p1 then p2（对齐 Concat.L 先算）
  Elem join(const Elem&, const Elem&) const;   // p1 + p2
  Elem star(const Elem&) const;                // p1*（闭式）
  fact_t apply(const Elem& summary, const fact_t& init) const; // 施加到初值
};
```

- `seq(L,R)` 语义务必与 `SolverContext::eval` 的 `Concat` 朝向一致：**`Concat.L` 先施加**。
  故 Gen/Kill：`Gen = Gen_R ∪ (Gen_L\Kill_R)`；矩阵：`M = M_R·M_L`。（写单测锁死朝向。）

### 3.3 两个具体半环

- **GenKillSemiring**：`Elem = {BitVector Gen, BitVector Kill}`，位宽 `|D|`。合成律见 §1.2。
  `apply((G,K), s) = G | (s & ~K)`。`star = {Gen, 0}`。
- **IFDSMatrixSemiring**：`Elem = {BitMatrix M, BitVector C}`（`M` 按行位打包，行=`BitVector`）。
  合成律见 §1.3；`star`=布尔传递闭包（Warshall，`O(|D|³)`，位并行按行或）+ `C* = M*·C`。
  `apply((M,C), s) = (M·s) | C`。

### 3.4 机械抽取器 `AtomTranslator`（论文“机械/语义无关”的落地）

利用 `⟦e⟧` 的**分配性**：`f(x) = f(∅) ∪ ⋃_{d∈x} f({d})`。给定有限事实全集 `U`（客户端提供枚举
+ 稳定下标），对每个唯一 `transfer_t` 原子探测现有 `applyTransfer`：

- **IFDS(M,C)**：`C = idx(applyTransfer(T, ∅))`；`M` 的第 `d` 列 = `idx(applyTransfer(T,{d}))`。
  共 `|U|+1` 次探测。
- **Gen/Kill 快路径**（separable 客户端）：`Gen = applyTransfer(T,∅)`；
  `Kill = { d∈U : d ∉ applyTransfer(T,{d}) }`。同为 `|U|+1` 次探测（或用 `apply(U)` 一次差分）。

要点：
- **完全机械**、复用客户端自己的 `applyTransfer`，无需逐客户端手写 Gen/Kill——契合论文主张。
- 按 `transfer_t` 缓存（同一指令原子只抽取一次）。
- 附带 **distributivity 自检**（随机若干 `x`，校验 `f(x)==C|⋃f({d})`）；不满足 ⇒ 该客户端不属
  目标类，基线跳过并诊断。
- 客户端只需新增一个薄适配 `FactUniverse`：`std::vector<Fact> universe()` + `index(Fact)`。
  Lotus 现有客户端（`ReachingDefinitions`=Value*/Instruction 集、`UninitVariables`=变量集、
  `Reachable`=单元素）都能廉价给出。

### 3.5 记忆化折叠解释器 `FoldInterpreter`

对 `PathExprFactory<transfer_t>::Ref` DAG 自底向上折叠，`unordered_map<const Expr*, Elem>` 记忆
（**每唯一节点算一次**，共享子表达式不重复）——这与通用 `eval` 的树遍历（按 `expandedTree` 展开
且星迭代）形成清晰的求值成本对比。

```
Elem fold(Ref e):
  Zero→zero; One→one; Atom→translator.get(*e->Transfer)
  Union→join(fold(L),fold(R)); Concat→seq(fold(L),fold(R)); Star→star(fold(L))
```

节点结果 IN facts = `apply(fold(ExprTo(N)), initialFact())`。

### 3.6 驱动与接线（复用前端，最小侵入）

- `Driver.h::runTranslApaBaseline(Problem, FactUniverse, Semiring, Opts)`：
  1. 用现有 `IntraEliminationSolver` 求解（拿 `ExprTo` 批 = `p`，不开 EAN/Greedy）；
  2. 抽取原子 → `FoldInterpreter` 折叠 → 填 `Results.IN(N)`；
  3. 记录 `TranslApaStats` + 折叠耗时（复用 `SolveDiagnostics` 的 `interp_time_us` 语义，或新增
     `translapa_*` 字段，避免污染现有列）。
- CLI：在 `tools/dataflow/lotus-dfa-apa.cpp` 增 `--interp={generic|translapa}`（或 `--baseline=translapa`），
  与现有 `--ean/--greedy` 并列；对 Gen/Kill vs IFDS 由客户端类型或 `--translapa-mode={genkill|ifds}` 选择。
  额外打印 `[translapa] elem_bits=…, matrix_nnz=…, fold_us=…`，与 `[dagstats]/[timing]` 同排，供
  `docs/eval/real_eval.py` 直接抓取对比。
- eval：`docs/eval/real_eval.py` 增列（现状 APA / EAN / TranslAPA 的节点数与耗时三方对比）。

### 3.7 差分正确性 oracle（论文的“correct by construction”给了免费 oracle）

论文保证翻译版与 IDA/现有 APA **逐节点事实相同**。故对每客户端每函数断言：
`facts_generic(N) == facts_translapa(N)`（以及 `== facts_EAN(N)`）。
接入 CLI 现有 RQ1 differential + 新增 `tests/unit/Dataflow/APA/Baseline/TranslAPATest.cpp`
（小型手写 CFG：`x:=1;while{if x:=2;y:=1 else y:=2};y:=3`，即论文 Fig.1，校验 reaching defs =
`{1,4,10}`，并校验星闭式与迭代 eval 结果一致）。

### 3.8 对比口径（回答任务）

- **DAG 节点数**：`p` 的 `uniqueNodes` 对三方相同（同一前端）；我们的 EAN 会**降低** `p` 的节点数，
  TranslAPA **不改 `p` 结构**，而是把“求值”折进闭式半环元素（星=1 次闭包 vs 迭代）。因此：
  - 报告 (a) EAN 前后 `p` 的 `uniqueNodes`（已有）；
  - 报告 (b) 三方**求值时间**：`eval(p)` / `eval(EAN(p))` / `TranslApaFold(p)`；
  - 报告 (c) TranslAPA 半环元素规模（矩阵 nnz / 位向量宽度）作为其“节点数”类比量。
- **命题**：在同一批路径表达式上，我方 EAN 的求值时间应能**匹配或优于**论文这一手工推导的
  最优闭式半环解释器——这正是“外部强基线”的意义。

---

## 4. 里程碑（建议）

| 里程碑 | 内容 | 交付 |
|---|---|---|
| **B0** | 本设计评审 + `FactUniverse` 适配接口敲定 | ✅ 本文档定稿 |
| **B1** | `Semiring` 概念 + `GenKillSemiring` + `FoldInterpreter` + `AtomTranslator`（探测法） | ✅ 单测 5/5：Fig.1 reaching defs `{1,4,10}`，fold==迭代 eval |
| **B2** | `Driver`(`foldFillGenKill`) + CLI `--interp=translapa` + `[interp]/[timing]/[dagstats]`；接 Reachable/ReachingDefinitions | ✅ bc14(libstdbuf) 差分一致（reachdef 139 行、reachable 全等）；驱动单测 4/4 |
| **B3** | `IFDSMatrixSemiring`（矩阵+Warshall）+ 稀疏优化；接 UninitializedVariables（非-Gen/Kill） | 论文三客户端对齐 |
| **B4** | 抽取快路径(可分离 `Kill=U\f(U)`) + 抽取/求值分列计时；`translapa_eval.py` 三方对比 + `RESULTS_TranslAPA.md` on bc14 | ✅ 3220 reachdef 函数：EAN 节点 0.27×、TranslAPA fold 0.69× 求值、**事实 0/99026 不等**；抽取 104µs vs EAN 饱和 838µs |
| **B5**（可选） | 增量场景对比（复用现有增量框架，验证 21×/2× 类结论是否复现） | 增量对比表 |

> **B2 遗留（转入 B4）**：机械抽取当前每原子探测 `|U|+1` 次（O(|U|²)/函数），在大链接模块（basename.bc）> 120s。B4 前置：可分离 Gen/Kill 快路径（`Kill = U \ f(U)`，每原子 2 次探测）+ 把一次性抽取耗时与逐查询 fold 耗时分列（对齐论文 construction vs evaluation）。

## 5. 风险与决策点

- **朝向一致性**：`seq`/矩阵乘方向必须与 `Concat.L 先算` 一致——B1 单测锁死。
- **抽取成本**：探测法 `O(|atoms|·|U|·apply)`；大函数 `|U|` 大时偏慢。缓解：Gen/Kill 走 `apply(U)`
  一次差分；IFDS 只在稀疏、按需列上探测。（这是基线**构造**成本，与其**求值**成本分列上报。）
- **全集来源**：需客户端给 `FactUniverse`。bool 型 Reachable 为退化单元素；建议主打
  ReachingDefinitions（Gen/Kill）+ UninitializedVariables（IFDS），与论文三基准对齐。
- **星非收敛**：闭式半环无此问题（是其优点）；但差分对照的通用 `eval` 可能触发
  `MaxStarIterations`——差分时对通用侧用足够大迭代上限或 `ReturnLast`。
- **命名**：基线放 `Dataflow/APA/Baseline/TranslAPA/`，`namespace elimination::translapa`，与
  `elimination::ean` 并列，避免与 EAN 混淆。

---

## 6. B0 决策（已确认）

1. ✅ 首个复刻子类：**Gen/Kill 先行**，客户端 ReachingDefinitions（+ 论文 Fig.1 合成 CFG 冒烟）。
2. ✅ 指标落点：**复用 `SolveDiagnostics::interp_time_us`**，在 CLI 以 tag（`[translapa]`）区分，
   不新增 `translapa_*` 字段。
3. ✅ 对比基准：**锁定 `bc14`**（仓库父目录 `D:/Code/lotus/bc14`），暂不引入 DaCapo。

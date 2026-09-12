# EAN 项目计划书

**项目名称**：EAN —— Equality-saturation Algebraic Normalizer（等式饱和代数规范化器）
**目标**：按照论文 `paper_apa_simp.pdf`，在 LOTUS 中实现一个位于**路径表达式构造**与**路径表达式解释**之间的编译器式优化阶段，用**等式饱和（equality saturation / e-graph）**对路径表达式这一"分析中间表示（IR）"做代数规范化与重用感知的抽取。
**一句话定位**：在 `Dataflow/APA` 产出的 `PathExprFactory` 表达式 DAG 与其解释器 `eval` 之间插入一个 `Ref → Ref` 的等价重写通道，底层复用已完成的 `Solvers/EGraph` 库。

> 说明：本文只做**规划**，不生成实现代码。所有"已实现"条目均给出真实文件路径与接口签名；所有"待实现"条目给出建议文件位置、接口草案与对应的论文章节/算法编号。

---

## 0. 关于论文里的 "tdb"（TBD）与你的职责

论文正文中大量出现 `[TBD: ...]`（To Be Determined）标记，集中在 **第 IV 节 Evaluation** 及 **表 IV–VIII**、以及摘要/结论里的数值。这些就是你负责的"tdb"部分：**实现 EAN 后，搭建评测流水线并把这些空缺填上**。它们不是散落的代码桩，而是"需要真实实验数据来回答的研究问题"。映射见 [第 8 节](#8-tdb--评测职责映射论文的所有-tbd)。

因此你的工作实际分两大块：
1. **实现 EAN 优化器本体**（第 4 节），使 APA 客户分析能启用它；
2. **实现评测基础设施**（第 4.11 节 + 第 8 节），产出论文所需的语义等价性、结构缩减、时间/内存、消融等数据。

---

## 1. 系统架构与数据流

论文图 1 的流水线，映射到 LOTUS 现有代码：

```
LLVM CFG/ICFG
   │  （已实现：Adapters/LLVM/*）
   ▼
路径表达式生成（状态消除 / ADT / 摘要方程）
   │  已实现：Solver/StateEliminationSolver.h、ADT*Solver.h、PathSummaryEquationSolver.h
   │  产物：PathExprFactory<TransferT> 里 hash-consed 的 Ref（Zero/One/Atom/Union/Concat/Star）
   ▼
★ EAN：规范化导入 → 有界饱和 → 批量 DAG 抽取  ★  ← 本项目要实现
   │  底层：Solvers/EGraph（已实现的 egg 风格 e-graph）
   │  产物：新的、等价的 PathExprFactory<TransferT> Ref（供解释器无感消费）
   ▼
代数解释（interpret）
   │  已实现：SolverContext::eval、InterSummaryTransferEvaluator::evaluateExpr
   ▼
分析结果（DataFlowResultT / InterDataFlowResultT）
```

**两个必须接入的集成边界（choke points）**：
- **Intra / 状态消除 / ADT**：`include/Dataflow/APA/Solver/SolverContext.h` 的 `eval(const expr_ref_t &E, const fact_t &In)`（约 712 行）。构造侧在 `materializeStateResults`（`StateEliminationSolver.h` 约 132–138 行）里执行 `Results.ExprTo(N)=E;` 紧接着 `Results.IN(N)=Ctx.eval(E,Init);`。
- **Inter / 摘要方程（前向）**：`include/Dataflow/APA/Solver/InterSummaryTransfer.h` 的 `InterSummaryTransferEvaluator::evaluateExpr`，由 `ForwardInterSummarySolver::evaluateSummaries` 在每个摘要 root 上调用。

> 关键结论：存在**统一的 construct-then-interpret 缝隙**。EAN 只需在"`ExprTo(N)` 赋值之后、`eval` 之前"插入一个 `Ref → Ref` 变换，即可对全部三种 intra 构造引擎生效；inter 侧需在 `evaluateExpr` 之前另接一次。

---

## 2. 论文要点回顾（实现契约）

- **输入契约**（第 III.A / II.C 节）：EAN 接收元组 `⟨R, L, C, B⟩`：`R`=一批 hash-consed 根表达式；`L`=客户**律法档案（law profile）**；`C`=抽取代价模型；`B`=资源预算。输出同格式的根批次 `R'`，满足 `L ⊢ r_i = r'_i`（式 4）。
- **三条需求**：
  - **R1 语义可容许**：每条重写都必须由客户实际满足的律法背书（能力门控）。
  - **R2 有界探索**：控制分配律/星号展开的爆炸；任意停止点都能给出有效可抽取表达式（anytime）。
  - **R3 消费者感知抽取**：代价要计入**共享导出 DAG**与**客户算子权重**，而非仅加性树大小。
- **三个不变式**（第 III.A）：I1 同余闭包、I2 档案相关的规范形（变长 JOIN 排序去重 / 变长 SEQ 展平不重排）、I3 根保留。
- **算法 1**（有界饱和）：`IMPORTCANONICAL → EXTRACTBATCH(best) →` 四个阶段 `[CLEANUP, FACTOR, STAR, EXPLORE]`，每轮 `MATCHGUARDED → APPLY → REBUILD → EXTRACTBATCH`，保留最优；`EXPORTHASHCONSED(best)`。
- **算法 2**（重用感知批量抽取）：先 cycle-safe 树抽取；迭代 K 次，构建共享 DAG、统计引用数、对被多次选中的类做**次线性折扣**、重抽取，取式 5 目标最优者。
- **式 5 目标**：`C_DAG = α·N_unique + β·E_unique + Σ_o w_o·N_o + γ·C_repeat`。
- **律法库**（表 I）：JOINACI、恒等、湮灭、左/右因式分解、星号规范化、星号展开、滑动（sliding）。能力：`JOINACI / LEFTDISTRIBUTIVE / RIGHTDISTRIBUTIVE / ANNIHILATION / KLEENESTAR / 左右展开 / sliding`。
- **预算**：`B = ⟨B_N, B_M, B_R, B_T, B_P⟩` = e-node 数、应用匹配数、轮数、墙钟时间、连续无改进轮数（plateau）。
- **失败安全**：任何资源失败则返回原始 `R`（靠 I3 根保留）。
- **正确性边界**：律法档案是客户契约，可对生成元做有限测试但不构成证明；错误声明属客户 bug，在优化器证明边界之外（Lemma 1、Lemma 2、Theorem 1）。

---

## 3. 已实现部分（可直接复用）

### 3.A EGraph 库 —— `lotus::egraph`（★ 已完成，论文所依赖的引擎）
位置：`include/Solvers/EGraph/*.h`（头文件为主）、`lib/Solvers/EGraph/`（`EGraph.cpp` 仅 7 行，构建目标 `LotusEGraph`）。umbrella：`include/Solvers/EGraph.h`。测试：`tests/unit/Solvers/EGraph*Test.cpp`。

模板参数贯穿始终：语言 `L` + 分析 `A`（默认 `NoAnalysis<L>`）。

| 头文件                                                 | 关键公共接口                                                                                                                                                                                                                                                                                            | 对 EAN 的意义                                        |
| --------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------ |
| `Id.h`                                              | `class Id`（`uint32_t` newtype）；`fromIndex/value/index`、序、`std::hash`                                                                                                                                                                                                                              | e-class / e-node 句柄                              |
| `UnionFind.h`                                       | `makeSet / find / findMut / unite / size`                                                                                                                                                                                                                                                         | I1 同余的并查集基础                                      |
| `Util.h`                                            | `Symbol`（全局串池）、`Duration/Instant/now()`、`hashCombine`、`UniqueQueue`                                                                                                                                                                                                                               | 算子命名、时间预算、worklist                               |
| `Language.h` / `DefineLanguage.h`                   | 语言概念（鸭子类型）：`children()`(**变长 `vector<Id>`**)、`discriminant()`、`matches()`、`mapChildren()`、`operator<`(**规范排序必需**)、`std::hash`；`LanguageOps<L>::{fromOp,display}` 特化；内置 `SymbolLang`、`DynamicLang`、`DefinedLang<Tag>`；宏 `LOTUS_EGRAPH_DEFINE_LANGUAGE` / `LOTUS_EGRAPH_LANG_OP`                    | **变长子节点 = 天然支持变长 JOIN/SEQ**；`operator<` 支撑 I2 排序 |
| `EClass.h`                                          | `EClass{id,nodes(排序去重),data,parents}`；`forEachMatchingNode`（<50 线性，否则按 discriminant 二分）                                                                                                                                                                                                           | e-class 表示；匹配依赖规范排序                              |
| `Analysis.h`                                        | `make/remake/merge/modify/preUnion/allowEMatchingCycles`；`DidMerge`、`NoAnalysis`、`mergeMax/Min/Option`、`Justification{Congruence,Rule}`                                                                                                                                                           | e-class 分析；`allowEMatchingCycles()` 是匹配侧环路门控     |
| `EGraph.h`                                          | `add / addUncanonical / addExpr / lookup / find / unite / uniteChecked / rebuild()`；`operator[]`、`classIds/classes/classesForOp`、`totalSize/numberOfClasses`；`egraphUnion/Intersect`、JSON、`LanguageMapper`；解释开关                                                                                   | e-graph 主体；`rebuild()`=同余闭包+每类 `sort+unique` 规范化 |
| `RecExpr.h` / `SExp.h`                              | `RecExpr<L>`（拓扑数组）：`add/root/items/extract/toString/parse`；`SExp::parse`                                                                                                                                                                                                                          | 表达式/DAG 表示与 S-表达式解析                              |
| `Subst.h`                                           | `Var`(`?x`)、`Subst`                                                                                                                                                                                                                                                                               | 模式变量与绑定                                          |
| `Pattern.h` / `PatternMachine.h` / `MultiPattern.h` | `Pattern<L>::{parse,search,searchWithLimit,...}`、`PatternProgram`(编译式 e-matcher，`runWithLimit`)、`MultiPattern`                                                                                                                                                                                    | 模式匹配；**所有匹配都带 limit**（单匹配预算钩子）                   |
| `Rewrite.h`                                         | `Searcher/Applier` 抽象；`Condition<L,A>=function<bool(EGraph&,Id,Subst)>`；`ConditionalApplier`、`ConditionEqual`；`Rewrite<L,A>`；`makeRewrite / makeConditionalRewrite / makeMultiRewrite`                                                                                                            | **条件重写 = 律法门控的机制**                               |
| `Runner.h`                                          | `RunnerLimits{iter_limit,node_limit,time_limit}`、`StopReason{Saturated/IterationLimit/NodeLimit/TimeLimit}`；`RewriteScheduler`、`SimpleScheduler`、`BackoffScheduler`(per-rule 匹配上限+指数封禁)；`Runner{withExpr/withEGraph/withIterLimit/withNodeLimit/withTimeLimit/withScheduler/withHook/run/report}` | **有界饱和已具备**：轮/节点/时间限 + per-rule 匹配预算             |
| `Extract.h`                                         | `CostFunction<Derived,L,Cost>` CRTP + `cost(node,child_cost)`；内置 `AstSize/AstDepth`；`Extractor<L,A,CostFn>::{findBest,findBestNode,findBestCost}`（构造期 `compute()` 做**逐类最优的自底向上不动点**）                                                                                                              | 代价抽取；DAG 感知代价传播已有，但见"缺口"                         |
| `Explain.h`                                         | `Explanation`、`FlatTerm/TreeTerm`、`explainEquivalence`、`checkProof`                                                                                                                                                                                                                               | 证明/差分测试可选支撑                                      |
| `Dot.h` / `Version.h`                               | Graphviz 导出（POSIX-only）、`version()`                                                                                                                                                                                                                                                               | 调试可视化                                            |

**真实用法示例**（`tests/unit/Solvers/EGraphMathTest.cpp`）：定义 `ConstantFold` 分析、`makeRewrite("comm-add","(+ ?a ?b)","(+ ?b ?a)")`、`Runner<...>().withExpr(RecExpr::parse(t)).withIterLimit(12).run(rules())`、`Extractor(...).findBest(root)`。

**EGraph 对 EAN 需求的支持度小结**：

| EAN 需求 | 现状 |
|---|---|
| 变长 JOIN/SEQ 节点 | ✅ 子节点即 `vector<Id>`（用 `SymbolLang`/`DynamicLang`） |
| 规范排序（I2） | ✅ `operator<` + `rebuild` 的 `sort+unique` |
| 律法门控重写（R1） | ✅ `Condition` / `ConditionalApplier` |
| 有界饱和（R2：节点/匹配/轮/时间/plateau） | ✅ 大部分：`RunnerLimits` + `BackoffScheduler`；plateau 需自加 hook |
| per-算子权重代价模型 | ⚠️ 部分：`CostFunction` 概念在，但无现成权重表（自写 cost fn 即可） |
| DAG/重用感知抽取（R3、式 5） | ⚠️ 部分：有逐类不动点代价，但导出是**树**、贪心局部，非 DAG 最优、无 α/β/γ/w_o、无重用折扣迭代 |
| Cycle-safe 抽取 | ❌ 贪心 `buildInto` 无环保护，环形最优选择会无限递归 |

### 3.B APA 框架 —— `namespace elimination`（★ 已完成，EAN 的上下游）
位置：`include/Dataflow/APA/*`、`lib/Dataflow/APA/*`（构建目标 `APADataFlow`）。umbrella：`include/Dataflow/APA/APA.h`。

**核心表达式工厂（EAN 的输入/输出类型）** —— `include/Dataflow/APA/Core/PathExpr.h`：
```cpp
template <typename TransferT> class PathExprFactory {
  enum class Kind { Zero, One, Atom, Union, Concat, Star };
  struct Expr { Kind K; shared_ptr<const TransferT> Transfer; shared_ptr<const Expr> L, R; };
  using Ref = shared_ptr<const Expr>;
  Ref zero()/one()/atom(T)/unite(A,B)/concat(A,B)/star(A);   // per-factory hash-consing
};
```
- 已内建**通用 Kleene 化简**（EAN 律法的前身/扩展点）：`unite`(`0⊕x=x`,`x⊕0=x`,`A⊕A=A`)、`concat`(`0·x=x·0=0`,`1·x=x`,`x·1=x`)、`star`(`0*=1*=1`,`(A*)*=A*`)。
- **二元** Union/Concat（非变长），指针键 hash-cons；无重结合/分配/跨节点因式分解——正是 EAN 要补的。

**客户代数接口（律法档案的挂靠点）** —— `include/Dataflow/APA/Core/Problem.h`：
```cpp
template <typename AnalysisDomainTy> class IntraEliminationProblem {
  using n_t/fact_t/transfer_t = ...;
  nodes()/entry()/succs();
  transfer_t edgeTransfer(n_t,n_t);  fact_t applyTransfer(transfer_t,fact_t);   // ·
  fact_t meet(fact_t,fact_t);        bool equal_to(fact_t,fact_t);              // ⊕
  fact_t meetIdentity();  fact_t initialFact();  size_t maxStarIterations();    // 0 / 1
};
```
语义映射：`Union→meet`、`Concat→applyTransfer 组合`、`Star→迭代 meet 至 equal_to 稳定`、`Zero→meetIdentity`、`One→返回 In`、`Atom(T)→applyTransfer(T,In)`。
Inter 版 `Core/InterProblem.h`：追加 `callFlow/returnFlow/callToRetFlow/getCalleesOfCallAt/initialSeeds`、`allTop`（inter 的 0）。

**求解器（构造引擎 + 解释边界）** —— `include/Dataflow/APA/Solver/`：
- `Solver.h` `IntraEliminationSolver`（门面，ADT 失败回退 StateElimination）。
- `SolverContext.h` `detail::IntraEliminationSolverContext`：持有唯一的 `expr_factory_t Exprs`、`Matrix`（消除矩阵）、`Results`；**解释入口 `eval(E,In)`（约 712 行）**。
- `StateEliminationSolver.h`：`buildStateEliminationMatrix → eliminateStateIntermediates`(`M[i][j]|=M[i][k]·M[k][k]*·M[k][j]`) `→ materializeStateResults`；**消除次序策略 `getStateEliminationOrder`**（可约图取逆拓扑，否则 0..N-1）。
- `ADTSimpleSolver.h` / `ADTDelayedSolver.h`：可约流图的 ADT 引擎（`SimpleExpr` 急切 / `evalUF` 延迟+路径压缩）。
- `PathSummaryEquationSolver.h`：左线性摘要方程 `X_u = base_u ⊕ (W_{u,v}·X_v)`，Tarjan SCC + SCC 内状态消除闭包（递归→`Star`）；**只构造不解释**，最纯的代数面，EAN 的理想消费者。
- `InterSolver.h` `InterEliminationSolver<...,K>`（call-string worklist，`mono::CallStringCTX`）。
- `InterSummaryTransfer.h` `InterSummaryTransferAtom{RawNormal,Normal,CallEntry,ReturnExit,CallToRet}` + `InterSummaryTransferEvaluator::evaluateExpr`（**第二个 DAG 解释器**）；`ForwardInterSummarySolver.h`。

**配置** —— `Core/Options.h`：`EliminationMethod / OnNonConvergentStar / SolveStatus / SolveDiagnostics / EliminationOptions`。**EAN 开关将加在此处**。

**结果** —— `Core/Result.h` `DataFlowResultT{IN(N), ExprTo(N)}`（**路径表达式是一等保留输出**）；`Core/InterResult.h`。

### 3.C 现有客户分析（EAN 的评测客户来源）
- Intra（`Analyses/Intra/`，10 个）：Reachability、ConstantPropagation、LiveVariables、ReachingDefinitions、AvailableExpressions、UninitializedVariables、Lockset、VeryBusyExpressions、NonNull、SignAnalysis。
- Inter（`Analyses/Inter/`，7 个）：Reachability、ConstantPropagation、LiveVariables、ReachingDefinitions、UninitializedVariables、Lockset、AffineEqualities。
- `Analyses/ExpressionKey.h`：交换算子/比较谓词的**客户级结构规范化**（`isCommutativeOpcode`、操作数排序）——律法档案的现成先例。
- `Domains/AffineRelationDomain.h`：更"重"的代数（`meet/combine/extend/extend_lin`），对应论文里"组合/闭包昂贵"的 transition 客户，评测 per-算子权重的好素材。
- LLVM 适配：`Adapters/LLVM/ForwardProblem.h` `LLVMIntraEliminationProblem<FactT>` 提供全部 CFG/支配者管线；`Passes/EliminationPasses.h` 十个 legacy `FunctionPass`（`-elim-*`）。

---

## 4. 待实现部分（EAN 本体 + 评测）

建议全部置于新目录 **`include/Dataflow/APA/EAN/`** 与 **`lib/Dataflow/APA/EAN/`**（紧邻 APA，属 `elimination::ean` 子命名空间），并新增构建目标 `EANOptimizer`（链接 `LotusEGraph`）。下表每项标注：对应论文章节/算法、建议文件、接口草案、复用点、缺口。

### 4.1 律法档案 LawProfile（R1）
- **论文**：III.B.c、表 I、Lemma/Theorem 的信任边界。
- **文件**：`include/Dataflow/APA/EAN/LawProfile.h`（纯头）。
- **接口草案**：
  ```cpp
  namespace elimination::ean {
  enum class Law { JoinACI, LeftDistributive, RightDistributive, Annihilation,
                   KleeneStar, LeftUnfold, RightUnfold, Sliding, Identity };
  struct LawProfile {
    std::bitset<N> enabled;
    bool has(Law) const; LawProfile& enable(Law); 
    static LawProfile kleeneAlgebra();   // 全开
    static LawProfile flowAlgebra();     // 保守：恒等 + 选定分配律，禁用星号律
  };
  // 可选：客户在 Problem/Domain 上声明 lawProfile()；或按分析名注册默认档案
  }
  ```
- **挂靠**：作为 `IntraEliminationProblem`/`AnalysisDomainTy` 的附属声明（类比 `meet`/`edgeTransfer`）。可选提供"对随机生成元有限测试律法"的 harness（III.B.d）。

### 4.2 EAN 语言定义（变长 JOIN/SEQ/STAR/ATOM/常量）（I2）
- **论文**：III.A 节点文法 `ATOM(e) | JOIN({..}) | SEQ([..]) | STAR(c)` + 常量 0/1。
- **文件**：`include/Dataflow/APA/EAN/PathLang.h`。
- **做法**：用 `SymbolLang`（变长、串名算子）承载 `join/seq/star/zero/one/atom<i>`；原子用稳定整数 id 映射到 `transfer_t`（见 4.3 的 AtomTable）。JOIN 走"排序去重"、SEQ 走"展平不重排"由导入与 rebuild 保证。
- **缺口提示**：`LOTUS_EGRAPH_DEFINE_LANGUAGE` 宏是"(op, 定长 arity)"白名单，**不适合单算子变长**；应直接用 `SymbolLang`/`DynamicLang`。

### 4.3 规范导入 ImportCanonical（I1/I2/I3、Lemma 1）
- **论文**：III.A 导入、算法 1 第 1 行。
- **文件**：`include/Dataflow/APA/EAN/Import.h`、`lib/.../EAN/Import.cpp`。
- **接口草案**：
  ```cpp
  struct AtomTable { /* transfer_t <-> 稳定 atomId 双向映射（原子不透明） */ };
  struct ImportResult { egraph::EGraph<PathLang, EanAnalysis> g;
                        std::vector<egraph::Id> roots;   // 与输入 R 一一对应（I3）
                        AtomTable atoms; };
  template <class TransferT>
  ImportResult importCanonical(const std::vector<typename PathExprFactory<TransferT>::Ref>& R,
                               const LawProfile& L, const PathExprFactory<TransferT>& F);
  ```
- **行为**：单次遍历 hash-consed DAG（memo：`Expr* → Id`），共享子节点只导入一次；JOIN 展平嵌套 choice/去 0/去重/排序，SEQ 展平嵌套/去 1/（档案允许时）湮灭。复用 `EGraph::add` + `rebuild()`。

### 4.4 律法门控重写库（表 I）（R1/R2）
- **文件**：`include/Dataflow/APA/EAN/Rewrites.h`、`lib/.../EAN/Rewrites.cpp`。
- **做法**：用 `makeRewrite`/`makeConditionalRewrite` 生成 `Rewrite<PathLang,EanAnalysis>`；每条规则用 `Condition` 检查 `LawProfile::has(...)`（能力门控）。
- **需要自定义 Applier（变长因式分解，III.B.b）**：二元分配律不足，需"按 JOIN 中 SEQ 子节点的首/尾 e-class 分组 → 前缀 trie / 反向后缀 trie 求最长公共前后缀 → 加 `P·(q1⊕…⊕qk)`"。这超出字符串模式能力，需实现自定义 `Applier` 子类（复用 `EGraph::add/addInstantiation/unite`）。按边界 e-class 建索引，仅对上轮变动的类重建索引。
- **规则清单**：恒等/湮灭（Cleanup）、左/右因式分解（Factor）、星号规范化/展开/滑动（Star）、反向分配/展开（Explore）。

### 4.5 有界、带守卫的饱和 Runner（算法 1、表 III、预算 B）（R2、Lemma 2）
- **文件**：`include/Dataflow/APA/EAN/Saturate.h`、`lib/.../EAN/Saturate.cpp`。
- **做法**：包装 `egraph::Runner` + 自定义 `RewriteScheduler`：
  - **相位调度** `[Cleanup, Factor, Star, Explore]`——现成 `Runner` 无相位概念，需在外层循环按相位切换规则集并复用 `Runner`/或自写等价 loop（`search→apply→rebuild→extractBatch→保留 best`）。
  - **预算 B**：`iter_limit`→B_R、`node_limit`→B_N、`time_limit`→B_T 复用 `RunnerLimits`；**B_M（应用匹配数）** 用 `BackoffScheduler` 的 per-rule 匹配上限或自定义调度器；**B_P（plateau，连续无抽取改进）** 用 `Runner::withHook` 监测 `EXTRACTBATCH` 代价实现。
  - **机会守卫** `H(m)=η_r·reuse+η_i·identity+η_s·starExpose−η_n·newNodes`（III.C）：作为 Explore 相位 `Condition`/自定义 `Applier` 的准入阈值。系数只排序候选、不参与正确性。
  - **环产生的星号展开**：仅在暴露恒等/对齐前后缀/使能 sliding 时准入（`allowEMatchingCycles()` 已允许匹配侧环）。

### 4.6 代价模型（式 5、per-算子权重）（R3）
- **文件**：`include/Dataflow/APA/EAN/CostModel.h`。
- **做法**：实现 `egraph::CostFunction` 子类，按 `node.op()`/`discriminant()` 给 `⊕/·/*` 不同权重 `w_o`；`Cost` 用可字典序的结构体承载 `(N_unique, E_unique, Σw_o·N_o, C_repeat)`。提供 `uniform` 与 `profiled` 两套配置（论文要求两种权重对照）。
- **复用**：`CostFunction<Derived,L,Cost>` 概念现成；缺口是"权重表"和"非加性 DAG 目标"，均在本模块自实现。

### 4.7 重用感知批量抽取 + Cycle-safe（算法 2、式 5）（R3）★ 主要缺口
- **文件**：`include/Dataflow/APA/EAN/Extract.h`、`lib/.../EAN/Extract.cpp`。
- **为何不能直接用 `egraph::Extractor`**：现成抽取器导出**树**、贪心局部、**无环保护**、无重用折扣迭代、无式 5 非加性目标。需**新实现**：
  ```cpp
  struct BatchExtractResult { /* 每 root 一个有限表达式 DAG（e-node 选择） */ };
  BatchExtractResult reuseAwareBatchExtract(EGraph&, const std::vector<Id>& roots,
                                            const CostModel& C, int K, double lambda);
  ```
  - **Cycle-safe 树抽取**：类代价初始化为 ∞，原子/常量置有限值，做**有界松弛**——仅当某 e-node 的所有子类已有有限派生时它才可选（保证构造有限表达式）；根类因 I3 恒有有限候选。
  - **重用迭代**：`best←cycleSafeTreeExtract` → 迭代 K：`BuildSharedDAG → ReferenceCounts → w_k(c)=w(c)/(1+λ·max(0,freq(c)−1))`（次线性折扣）→ 重抽取 → 按 `C_DAG` 保留最优；选择不变则提前停。
- **缺口**：`egraph` 无 DAG-最优/ILP 抽取、无 cycle-safe 抽取——**本模块是全新实现**（可参考 `Extractor::compute` 的不动点骨架，但需加环保护与共享折扣）。

### 4.8 导出回 PathExprFactory（Theorem 1 的 export）
- **文件**：`include/Dataflow/APA/EAN/Export.h`。
- **接口草案**：
  ```cpp
  template <class TransferT>
  typename PathExprFactory<TransferT>::Ref
  exportToFactory(const BatchExtractResult&, Id root, const AtomTable&,
                  PathExprFactory<TransferT>& F);  // 变长 JOIN→左折叠 unite；SEQ→左折叠 concat；STAR→star；atom→F.atom(...)
  ```
- **要点**：共享 e-class → 共享 factory 节点（跨 root 重用穿过 e-graph 边界）；不泄露任何 e-class id / 证书。因 `Ref` 不可变、按子指针 interning，导出必须自底向上重建。

### 4.9 顶层门面 + 失败安全
- **文件**：`include/Dataflow/APA/EAN/EAN.h`（umbrella）、`lib/.../EAN/EAN.cpp`。
- **接口草案**：
  ```cpp
  template <class TransferT>
  std::vector<typename PathExprFactory<TransferT>::Ref>
  ean(const std::vector<typename PathExprFactory<TransferT>::Ref>& R,
      const LawProfile& L, const CostModel& C, const Budget& B,
      PathExprFactory<TransferT>& F);   // import→saturate→extract→export；任何资源失败→返回 R（I3 兜底）
  ```

### 4.10 与 APA 求解器的接线（集成）
- **改动点**：
  - `Core/Options.h`：`EliminationOptions` 增 `bool EnableEAN=false; LawProfile EANLaws; CostModel EANCost; Budget EANBudget;`（或独立 `EanOptions`）。
  - `Solver/SolverContext.h`：暴露/传入 `Exprs`（当前私有），在 `materializeStateResults` 收集"本次 solve 的 root 批次"后、`eval` 前调用 `ean(...)`；或在 `eval` 头部对单 root 优化（批量更优）。
  - `Solver/ForwardInterSummarySolver.h`：在 `evaluateSummaries` 里对 `PathSummaryEquationResult` 批次先跑 `ean(...)` 再 `evaluateExpr`。
  - `Passes/EliminationPasses.h`：加 `-elim-ean`、`-elim-ean-laws=...`、`-elim-ean-budget=...` 等命令行开关（对照论文配置 Default/Greedy/Order/EAN/Order+EAN）。
- **构建**：`lib/Dataflow/APA/EAN/CMakeLists.txt` 新目标 `EANOptimizer`，`APADataFlow` 链接它与 `LotusEGraph`；在 `lib/Dataflow/APA/CMakeLists.txt` `add_subdirectory(EAN)`。

### 4.11 评测基础设施（★ 你的 "tdb" 主战场）
- **文件**：`tools/ean-eval/`（命令行驱动）、`benchmarks/ean/`（语料清单 + 合成家族生成器）、`tests/unit/Dataflow/APA/EAN/*Test.cpp`。
- **能力**：
  - 五种配置：Default / Greedy（确定性局部化简，无候选保留）/ Order（代价感知消除次序，无后处理）/ EAN / Order+EAN（表 V）。
  - 指标采集：峰值 e-node、每轮 e-class/e-node、按规则族的匹配数、stop 原因、导入/匹配/rebuild/抽取时间、最终唯一 DAG 节点/边、树大小、深度、`⊕/·/*` 计数、sharing、generation/normalization/interpretation/end-to-end 时间、峰值 RSS（表 VI/VII）。
  - RQ1 语义等价：对每个 subject×client×root 比较**客户事实**（非指针/语法）与 Default；外加对随机 DAG + 随机律法档案的差分测试（可用 `Explain::checkProof` 辅助）。
  - RQ4 消融：关闭 factorization / star / guarded expansion / phase schedule；uniform vs profiled tree cost vs profiled reuse-aware DAG cost；预算扫描。
  - 合成家族：重复前缀、重复后缀、分支深度、循环嵌套、跨 root 重用。

---

## 5. 新增文件与接口总览（一次看全"未来文件位置"）

| 模块 | 头文件 | 实现 | 论文对应 |
|---|---|---|---|
| 律法档案 | `include/Dataflow/APA/EAN/LawProfile.h` | （纯头） | III.B.c / 表 I |
| 语言 | `include/Dataflow/APA/EAN/PathLang.h` | （纯头） | III.A 文法 |
| 规范导入 | `include/Dataflow/APA/EAN/Import.h` | `lib/Dataflow/APA/EAN/Import.cpp` | III.A / Lemma 1 / 算法1:1 |
| 重写库 | `include/Dataflow/APA/EAN/Rewrites.h` | `lib/Dataflow/APA/EAN/Rewrites.cpp` | 表 I / III.B.b |
| 饱和 | `include/Dataflow/APA/EAN/Saturate.h` | `lib/Dataflow/APA/EAN/Saturate.cpp` | 算法1 / 表 III / 预算 B |
| 代价模型 | `include/Dataflow/APA/EAN/CostModel.h` | （纯头/小 cpp） | 式 5 |
| 批量抽取 | `include/Dataflow/APA/EAN/Extract.h` | `lib/Dataflow/APA/EAN/Extract.cpp` | 算法 2 / cycle-safe |
| 导出 | `include/Dataflow/APA/EAN/Export.h` | （纯头/小 cpp） | Theorem 1 export |
| 门面 | `include/Dataflow/APA/EAN/EAN.h` | `lib/Dataflow/APA/EAN/EAN.cpp` | III 全流程 |
| 构建 | `lib/Dataflow/APA/EAN/CMakeLists.txt` | — | — |
| 评测工具 | `tools/ean-eval/*` | — | 第 IV 节 |
| 单测 | `tests/unit/Dataflow/APA/EAN/*Test.cpp` | — | RQ1–RQ4 |

**需要修改的现有文件**：`include/Dataflow/APA/Core/Options.h`（EAN 开关）、`include/Dataflow/APA/Solver/SolverContext.h`（暴露工厂 + 接线）、`include/Dataflow/APA/Solver/ForwardInterSummarySolver.h`（inter 接线）、`include/Dataflow/APA/Passes/EliminationPasses.h` + `lib/.../Passes/EliminationPasses.cpp`（命令行）、`lib/Dataflow/APA/CMakeLists.txt`（子目录）。

---

## 6. 关键缺口与风险

1. **抽取是最大工作量**（4.7）：`egraph::Extractor` 既非 DAG 最优、也无 cycle-safe，需全新实现算法 2 + 式 5。
2. **变长因式分解需自定义 Applier**（4.4）：字符串模式无法表达"最长公共前/后缀分组"，要写 trie 索引 + `Applier` 子类。
3. **相位调度**（4.5）：`Runner` 无相位概念，需外层驱动或自写 loop；plateau（B_P）需 hook 自实现。
4. **工厂所有权**：`SolverContext::Exprs` / `PathSummaryEquationGraph::exprs()` 目前私有，接线需暴露或传入。
5. **两个解释器**：intra `SolverContext::eval` 与 inter `InterSummaryTransferEvaluator::evaluateExpr` 都要接。
6. **平台**：`Dot.h` 仅 POSIX（可视化调试用，非必需）。
7. **正确性边界**：律法档案错误声明属客户 bug（论文明确排除在证明外）；但需提供有限测试 harness 降低误用。

---

## 7. 建议里程碑（增量、每步可测）

- **M1 打通最小闭环**：`PathLang` + `importCanonical` + `exportToFactory` + 恒等/湮灭 Cleanup 规则 + 复用 `egraph::Extractor` 的树抽取；在一个 client（Reachability）上做到"导入→导出=语义不变"。（验证 I1/I3、Lemma 1）
- **M2 因式分解**：自定义变长前/后缀 `Applier` + Factor 相位；跑通论文式 2 的 running example。
- **M3 有界饱和**：相位调度 + 预算 B + 守卫 H；接 `Runner`/`BackoffScheduler`。
- **M4 重用感知抽取**：算法 2 + cycle-safe + 式 5 代价（uniform/profiled）。（R3）
- **M5 星号族**：星号规范化/展开/滑动 + 环安全导出。
- **M6 接线**：Options 开关 + SolverContext/ForwardInter 接入 + Pass 命令行；五配置可跑。
- **M7 评测**：`tools/ean-eval` + 合成家族 + RQ1–RQ4 采集 → 填补论文 TBD。

---

## 8. "tdb" / 评测职责映射（论文的所有 TBD）

| 论文位置 | TBD 内容 | 由什么产出 |
|---|---|---|
| 摘要 / 结论 | corpus size、client count、node/interpretation/end-to-end 缩减、best result | M7 汇总 |
| 表 IV | 语料规模（programs/functions/IR/roots/DAG/star 节点） | `benchmarks/ean` 语料统计脚本 |
| 表 V | 五配置定义 | 4.11 配置实现（已在本计划固定） |
| 表 VI | 相对 Default 的结构复杂度（unique/edges/tree/seq/star/sharing） | M4/M7 结构指标采集 |
| 表 VII | generation/normalization/interpretation/end-to-end/RSS/timeouts | M6/M7 时间与内存采集 |
| 表 VIII | 消融（去 factorization/star/guarded/phase；uniform/profiled tree cost） | M7 消融实验 |
| RQ1 §IV.B | 语义等价（unequal facts / missing roots / 差分测试数）、缩减幅度 | RQ1 差分测试 + 结构缩减 |
| RQ2 §IV.C | 解释时间/端到端/内存、break-even 阈值、invocation gate | RQ2 端到端计时 |
| RQ3 §IV.D | 消除次序 vs EAN 互补性、2×2、Spearman ρ | RQ3（需先接 Order 配置） |
| RQ4 §IV.E | 各机制贡献、预算膝点 | RQ4 消融 + 预算扫描 |
| §IV.A | 硬件/重复次数/超时/客户名单/权重来源 | 评测协议文档 |

---

## 9. 参考落点速查

- 论文：`../paper_apa_simp.pdf`
- e-graph 引擎：`include/Solvers/EGraph/`（`Runner.h`/`Extract.h`/`Rewrite.h`/`Language.h` 最关键）
- 表达式工厂（EAN I/O）：`include/Dataflow/APA/Core/PathExpr.h`
- 客户代数：`include/Dataflow/APA/Core/Problem.h`、`Core/InterProblem.h`
- 构造引擎：`include/Dataflow/APA/Solver/StateEliminationSolver.h`、`PathSummaryEquationSolver.h`
- 解释边界：`include/Dataflow/APA/Solver/SolverContext.h`（`eval`）、`Solver/InterSummaryTransfer.h`（`evaluateExpr`）
- 配置：`include/Dataflow/APA/Core/Options.h`
- 现有客户示例：`lib/Dataflow/APA/Analyses/Intra/IntraReachable.cpp`、`Analyses/ExpressionKey.h`

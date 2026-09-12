# E6 模块化过程间求解器 — 真实语料评测 (step③)

日期: 2026-09-12
分支: feat/rebuild-egg
工具: `lotus-dfa-apa.exe --analysis=inter_reachable`
语料: `bc14/coreutils`（LLVM 14, `--entry-function=main`）
超时: 每次调用 60s

四配置矩阵：
- `mod_noean`  = `--modular-inter`（E6 模块化，per-proc 摘要，functional/context-insensitive）
- `mod_ean`    = `--modular-inter --ean`（+ per-procedure EAN，D4）
- `whole_noean`= `--inter-summary`（整程序 ForwardInterSummarySolver，K=2 call-string）
- `whole_ean`  = `--inter-summary --ean`

原始数据: `docs/eval/raw/`（本次直接记录于 `/tmp/step3_results.tsv`，指标见下表）。

---

## 1. 关键发现：两堵墙

### R1/R3（过程间上下文爆炸）+ R2-inter（整程序×上下文树膨胀）— **模块化已消除**

模块化构造把每个过程在其自身 CFG 上求一次 entry→exit 摘要，调用点变为符号 `SummaryCall` 原子而**不内联**，因此不产生整程序×上下文乘积。对 main 可达集不含大环过程的程序，收益巨大（tree = expanded-tree occurrence，R2 的直接代理；elapsed = 端到端墙钟）：

| 程序 | mod tree | whole tree | tree 比 (whole/mod) | mod µs | whole µs | 加速 (whole/mod) | oracle diff |
|---|---|---|---|---|---|---|---|
| true     | 6.28e4 | 1.07e6 | **17.1×**   | 6,980   | 20,793    | 3.0×   | 0 |
| false    | 6.28e4 | 1.07e6 | **17.1×**   | 7,122   | 21,421    | 3.0×   | 0 |
| dirname  | 1.03e5 | 2.48e8 | **2397×**   | 12,665  | 3,271,077 | **258×** | 0 |
| printenv | 2.35e6 | 2.72e7 | 11.6×       | 68,479  | 397,831   | 5.8×   | 0 |
| runcon   | 4.29e6 | 2.21e7 | 5.1×        | 120,975 | 272,525   | 2.3×   | 0 |
| yes      | 9.29e5 | 2.90e8 | **312×**    | 36,360  | 3,813,609 | **105×** | 0 |
| echo     | 3.99e7 | **TIMEOUT(>60s)** | — | 755,304 | — | modular-only | (whole 未完成，无 oracle) |

- **正确性（D1-oracle）**：对每个 whole 能完成的程序，逐指令比对 modular 与 whole 的可达事实，**全部 diff=0**（健全且精确一致）。非递归/递归混合的真实调用图上，模块化 functional 求解与 K=2 worklist 存在性等价。
- **echo**：整程序模式 60s 超时，模块化 755ms 完成——一条干净的"whole 超时 / modular 完成"数据点。

### R2-intra（过程内稠密 Kleene 闭包）— **两种模式都撞的残余墙**

模块化**不**消除单个过程内部的 O(N³) 路径表达式闭包。经 `MOD_TRACE` 定位：coreutils 里存在一个约 152 节点 cyclic-SCC 的大过程（`quotearg_buffer_restyled` 家族，2188 指令），其单过程求解就耗 **7.5s**。凡 main 可达此过程的程序（多数非平凡 coreutils），modular 与 whole **两种模式都超时**（whole 更糟，还要乘上下文数）。

对最小的 35 个程序探测（8s 上限），仅 7 个的 main 可达集避开该大环、modular 能完成：true, false, dirname, printenv, runcon, yes, echo。其余（mktemp, unexpand, nproc, expand, kill, pwd, sleep, basename, uname, tty, …）modular 亦超时。

**结论**：E6 的收益来自**模块化构造本身**对过程间爆炸的消除，而非额外的 EAN 折叠；R2-intra 是与本工作正交的残余可扩展性瓶颈（过程内闭包近似/上界化是后续独立工作）。

---

## 2. per-procedure EAN (D4) 的作用

D4 在每个过程的小摘要森林上跑一遍 EAN。observed：

| 程序 | mod_noean tree | mod_ean tree | tree 降幅 | mod_noean nodes | mod_ean nodes |
|---|---|---|---|---|---|
| true     | 6.28e4 | 4.38e4 | −30% | 1033 | 1250 |
| dirname  | 1.03e5 | 7.08e4 | −31% | 1760 | 2553 |
| printenv | 2.35e6 | 8.71e5 | −63% | 2679 | 3224 |
| runcon   | 4.29e6 | 2.10e6 | −51% | 2191 | 2742 |
| yes      | 9.29e5 | 1.98e5 | −79% | 2352 | 4045 |
| echo     | 3.99e7 | 1.08e7 | −73% | 5157 | 5690 |

- per-proc EAN 稳定降低 expanded-tree（−30%…−79%），**但 unique-node 略升**：每过程摘要森林很小，跨根共享空间有限，EAN 用局部展开换取占位数下降。
- **正确性**：`mod_ean` 与 `mod_noean` 逐指令可达事实一致（0 差异，单测 `PerProcedureEANPreservesFacts` + 语料实测均验证）。EAN 保语义。
- ROI 判断：D4 对 tree（解释期开销代理）有效，对 DAG 节点（内存代理）在小批次上不利；是否默认开启取决于下游是否解释期敏感。

---

## 3. 修复的健全性 bug（本次发现）

初次语料对拍时，dirname/printenv/runcon 出现 **modular 少报 248 条可达指令**（全部集中在递归函数 `version_etc_arn`）。

- 根因：`ModularInterSummaryDriver` 外层定点的收敛信号 `Interp.changedThisPass()` **只观测解释器递归 memo（procApply）**，看不见驱动 `propagate` 对跨过程 `EntryFact` 的更新。调用链 `main→version_etc→version_etc_va→version_etc_arn` 需逐跳（callee-first 序，每 pass 一跳）传播；递归 memo 一旦稳定即提前 break，最深的 `version_etc_arn` 留在 unreachable → 不健全欠近似。
- 修复：`propagate` 返回"本 pass 是否有 EntryFact 变化"，外层 `break` 需同时满足 `!EntryChanged && !changedThisPass`。
- 回归测试：`ModularInterSummaryDriver.ReachabilityFlowsDeepCallChain`（main→A→B→C 深度 3 链）。
- 修复后：6 个可比程序全部 diff=0。

---

## 4. 复现

```sh
BIN=build/bin/lotus-dfa-apa.exe
BC=bc14/coreutils/dirname.bc
$BIN --analysis=inter_reachable --modular-inter        --entry-function=main --stdout $BC   # E6
$BIN --analysis=inter_reachable --modular-inter --ean  --entry-function=main --stdout $BC   # E6 + D4
$BIN --analysis=inter_reachable --inter-summary         --entry-function=main --stdout $BC   # whole-program
```

`[inter-summary]` / `[dagstats-before|after]` 行给出 contexts / eqn_nodes / gen_us / interp_us / nodes / tree。

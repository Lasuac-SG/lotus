/*
 * lotus-dfa-apa
 *
 * Dataflow testing tool: APA (Algebraic Program Analysis) engine.
 */

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/APA/Analyses/Inter/InterConstantPropagation.h"
#include "Dataflow/APA/Analyses/Inter/InterLiveVariables.h"
#include "Dataflow/APA/Analyses/Inter/InterReachability.h"
#include "Dataflow/APA/Analyses/Inter/InterReachingDefinitions.h"
#include "Dataflow/APA/Analyses/Inter/InterUninitializedVariables.h"
#include "Dataflow/APA/Analyses/Intra/IntraAvailableExpressions.h"
#include "Dataflow/APA/Analyses/Intra/IntraConstantPropagation.h"
#include "Dataflow/APA/Analyses/Intra/IntraLiveVariables.h"
#include "Dataflow/APA/Analyses/Intra/IntraReachability.h"
#include "Dataflow/APA/Analyses/Intra/IntraReachingDefinitions.h"
#include "Dataflow/APA/Analyses/Intra/IntraUninitializedVariables.h"
#include "Dataflow/APA/EAN/DagStats.h"
#include "ToolSupport.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// psapi.h must follow windows.h
#include <psapi.h>
// windows.h defines IN/OUT as empty SAL macros, which would mangle the solver
// result accessor Result.IN(...). Drop them.
#undef IN
#undef OUT
#else
#include <sys/resource.h>
#endif

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<bitcode>"),
                                          cl::Required);
static cl::opt<std::string> OutDir("out-dir", cl::desc("Output directory"),
                                   cl::value_desc("dir"), cl::init(""));
static cl::opt<bool> StdoutOpt(
    "stdout",
    cl::desc("Write analysis results to stdout when --out-dir is not set"),
    cl::init(false));
static cl::opt<std::string> AnalysisOpt(
    "analysis",
    cl::desc("Analysis: liveness (default), reaching_defs, uninitialized, "
             "constant_prop, available_exprs, reachable, inter_liveness, "
             "inter_reaching_defs, inter_uninitialized, inter_constant_prop, "
             "inter_reachable"),
    cl::init("liveness"));
static cl::opt<std::string>
    EntryFunctionOpt("entry-function",
                     cl::desc("Entry function for interprocedural analyses"),
                     cl::init("main"));
static cl::opt<std::string> ElimMethodOpt(
    "elim-method",
    cl::desc("Elimination solver method: state|adt-simple|adt-delayed"),
    cl::init("state"));
static cl::opt<bool>
    DumpProfileOpt("dump-profile",
                   cl::desc("Dump solver and path-expression profiling data"),
                   cl::init(false));
static cl::opt<bool>
    DumpExprsOpt("dump-exprs",
                 cl::desc("Dump per-instruction path-expression summaries"),
                 cl::init(false));

// --- EAN / Order (evaluation) configuration ---------------------------------
static cl::opt<std::string>
    OrderingOpt("ordering",
                cl::desc("Pivot order for state elimination: default|cost-aware"),
                cl::init("default"));
static cl::opt<bool> EanOpt("ean",
                            cl::desc("Run the EAN normalizer post-pass"),
                            cl::init(false));
static cl::opt<std::string>
    EanLawsOpt("ean-laws", cl::desc("EAN law profile: safe|kleene"),
               cl::init("safe"));
static cl::opt<unsigned>
    EanRoundLimit("ean-round-limit",
                  cl::desc("EAN saturation round budget (0 = unbounded)"),
                  cl::init(0));
static cl::opt<unsigned>
    EanNodeLimit("ean-node-limit",
                 cl::desc("EAN e-node budget (0 = unbounded)"), cl::init(0));
static cl::opt<double>
    EanTimeLimit("ean-time-limit",
                 cl::desc("EAN wall-clock budget in seconds (0 = unbounded)"),
                 cl::init(0.0));
static cl::opt<bool>
    MeasurePeakOpt("measure-peak",
                   cl::desc("Record peak construction nodes (slower)"),
                   cl::init(false));
static cl::opt<unsigned>
    MaxFuncInsts("max-func-insts",
                 cl::desc("Skip functions with more instructions (0 = no cap)"),
                 cl::init(0));
static cl::opt<unsigned>
    RepeatOpt("repeat", cl::desc("Measured runs per function (timing median)"),
              cl::init(1));
static cl::opt<unsigned>
    WarmupOpt("warmup", cl::desc("Warmup runs per function before measuring"),
              cl::init(0));

namespace {

using lotus::dataflow_tool::FunctionView;
using lotus::dataflow_tool::ValueIdMap;
using InstructionExprFactory = elimination::PathExprFactory<Instruction *>;
using InstructionExprRef = InstructionExprFactory::Ref;

// Aggregated stage timings (microseconds) across the measured repeats.
struct Timings final {
  std::uint64_t gen = 0;
  std::uint64_t norm = 0;
  std::uint64_t interp = 0;
  std::uint64_t end2end = 0;
  unsigned runs = 1;
};

std::uint64_t medianOf(std::vector<std::uint64_t> V) {
  if (V.empty())
    return 0;
  std::sort(V.begin(), V.end());
  return V[V.size() / 2];
}

// Peak resident memory of this process in KiB (Table VII Peak RSS).
std::uint64_t peakRssKb() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS PMC;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &PMC, sizeof(PMC)))
    return static_cast<std::uint64_t>(PMC.PeakWorkingSetSize) / 1024;
  return 0;
#elif defined(__APPLE__)
  struct rusage RU;
  if (getrusage(RUSAGE_SELF, &RU) == 0)
    return static_cast<std::uint64_t>(RU.ru_maxrss) / 1024; // bytes on macOS
  return 0;
#else
  struct rusage RU;
  if (getrusage(RUSAGE_SELF, &RU) == 0)
    return static_cast<std::uint64_t>(RU.ru_maxrss); // KiB on Linux
  return 0;
#endif
}

// Build EliminationOptions from the evaluation CLI flags.
elimination::EliminationOptions buildElimOpts() {
  auto Opts = lotus::dataflow_tool::parseEliminationOptions(ElimMethodOpt);
  if (OrderingOpt == "cost-aware")
    Opts.Ordering = elimination::OrderingPolicy::CostAware;
  Opts.EnableEAN = EanOpt;
  Opts.EANLaws = (EanLawsOpt == "kleene")
                     ? elimination::ean::LawProfile::kleeneAlgebra()
                     : elimination::ean::LawProfile::safeMinimal();
  elimination::ean::Budget B = elimination::ean::Budget::unbounded();
  if (EanRoundLimit)
    B.roundLimit = EanRoundLimit;
  if (EanNodeLimit)
    B.nodeLimit = EanNodeLimit;
  if (EanTimeLimit > 0.0)
    B.timeLimitSec = EanTimeLimit;
  Opts.EANBudget = B;
  Opts.MeasurePeakNodes = MeasurePeakOpt;
  return Opts;
}

ValueIdMap buildModuleValueIdMap(Module &M) {
  ValueIdMap ValueToId;
  for (auto &G : M.globals()) {
    ValueToId[&G] = (G.hasName() ? G.getName() : "global").str();
  }

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    const std::string Prefix = F.getName().str();
    unsigned ArgIdx = 0;
    for (auto &Arg : F.args()) {
      ValueToId[&Arg] = Prefix + ".arg" + std::to_string(ArgIdx++);
    }

    unsigned InstIdx = 0;
    for (auto &BB : F) {
      for (auto &I : BB) {
        ValueToId[&I] = Prefix + ".i" + std::to_string(InstIdx++);
      }
    }
  }
  return ValueToId;
}

template <typename ContextT> std::string contextToString(const ContextT &Ctx) {
  std::string Buffer;
  raw_string_ostream OS(Buffer);
  Ctx.print(OS);
  return OS.str();
}

const char *toString(elimination::EliminationMethod M) {
  switch (M) {
  case elimination::EliminationMethod::StateElimination:
    return "state";
  case elimination::EliminationMethod::ADTSimple:
    return "adt-simple";
  case elimination::EliminationMethod::ADTDelayed:
    return "adt-delayed";
  }
  return "unknown";
}

const char *toString(elimination::SolveStatus S) {
  switch (S) {
  case elimination::SolveStatus::Ok:
    return "ok";
  case elimination::SolveStatus::FallbackToState:
    return "fallback-to-state";
  case elimination::SolveStatus::NonConvergentStar:
    return "non-convergent-star";
  case elimination::SolveStatus::InvalidProblem:
    return "invalid-problem";
  }
  return "unknown";
}

const char *toString(elimination::FallbackReason R) {
  switch (R) {
  case elimination::FallbackReason::None:
    return "none";
  case elimination::FallbackReason::ADTRejected:
    return "adt-rejected";
  case elimination::FallbackReason::InvalidProblem:
    return "invalid-problem";
  }
  return "unknown";
}

std::string formatExpressionKey(const elimination::ExpressionKey &Key) {
  std::ostringstream ss;
  ss << "op" << Key.Opcode << "(";
  for (size_t i = 0; i < Key.Ops.size(); ++i) {
    if (i)
      ss << ",";
    ss << Key.Ops[i];
  }
  ss << ")";
  return ss.str();
}

std::string formatValueLatticeElement(const ValueLatticeElement &Val) {
  std::ostringstream ss;
  if (Val.isUndef())
    ss << "undef";
  else if (Val.isUnknown())
    ss << "unknown";
  else if (Val.isOverdefined())
    ss << "overdefined";
  else if (Val.isNotConstant())
    ss << "notconst";
  else if (Val.isConstant()) {
    if (auto *CI = dyn_cast<ConstantInt>(Val.getConstant()))
      ss << "const" << CI->getZExtValue();
    else
      ss << "const";
  } else
    ss << "lattice";
  return ss.str();
}

struct CFGStats final {
  size_t Arguments = 0;
  size_t Blocks = 0;
  size_t Instructions = 0;
  size_t Edges = 0;
  size_t BranchingBlocks = 0;
  size_t MaxSuccessors = 0;
  size_t PhiNodes = 0;
  size_t Calls = 0;
  size_t Returns = 0;
  size_t Unreachable = 0;
};

CFGStats collectCFGStats(const Function &F) {
  CFGStats Stats;
  Stats.Arguments = F.arg_size();
  for (const auto &BB : F) {
    ++Stats.Blocks;
    Stats.Instructions += BB.size();
    for (const auto &I : BB) {
      if (isa<PHINode>(I))
        ++Stats.PhiNodes;
      if (isa<CallBase>(I))
        ++Stats.Calls;
      if (isa<ReturnInst>(I))
        ++Stats.Returns;
      if (isa<UnreachableInst>(I))
        ++Stats.Unreachable;
    }
    const size_t Succs = succ_size(&BB);
    Stats.Edges += Succs;
    Stats.MaxSuccessors = std::max(Stats.MaxSuccessors, Succs);
    if (Succs > 1)
      ++Stats.BranchingBlocks;
  }
  return Stats;
}

struct ExprProfile final {
  size_t UniqueNodes = 0;
  size_t SharedRefs = 0;
  size_t MaxDepth = 0;
  size_t ZeroNodes = 0;
  size_t OneNodes = 0;
  size_t AtomNodes = 0;
  size_t UnionNodes = 0;
  size_t ConcatNodes = 0;
  size_t StarNodes = 0;
};

void collectExprProfileImpl(const InstructionExprRef &Expr, size_t Depth,
                            std::unordered_set<const void *> &Visited,
                            ExprProfile &Profile) {
  if (!Expr)
    return;

  Profile.MaxDepth = std::max(Profile.MaxDepth, Depth);
  if (!Visited.insert(Expr.get()).second) {
    ++Profile.SharedRefs;
    return;
  }

  ++Profile.UniqueNodes;
  switch (Expr->K) {
  case InstructionExprFactory::Kind::Zero:
    ++Profile.ZeroNodes;
    return;
  case InstructionExprFactory::Kind::One:
    ++Profile.OneNodes;
    return;
  case InstructionExprFactory::Kind::Atom:
    ++Profile.AtomNodes;
    return;
  case InstructionExprFactory::Kind::Union:
    ++Profile.UnionNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    collectExprProfileImpl(Expr->R, Depth + 1, Visited, Profile);
    return;
  case InstructionExprFactory::Kind::Concat:
    ++Profile.ConcatNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    collectExprProfileImpl(Expr->R, Depth + 1, Visited, Profile);
    return;
  case InstructionExprFactory::Kind::Star:
    ++Profile.StarNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    return;
  }
}

ExprProfile collectExprProfile(const InstructionExprRef &Expr) {
  ExprProfile Profile;
  std::unordered_set<const void *> Visited;
  collectExprProfileImpl(Expr, 1, Visited, Profile);
  return Profile;
}

std::string formatTransfer(const Instruction *I, const ValueIdMap &ValueToId) {
  if (!I)
    return "null";
  auto It = ValueToId.find(I);
  return It != ValueToId.end() ? It->second : "inst";
}

void formatPathExpr(raw_ostream &OS, const InstructionExprRef &Expr,
                    const ValueIdMap &ValueToId) {
  if (!Expr) {
    OS << "null";
    return;
  }

  switch (Expr->K) {
  case InstructionExprFactory::Kind::Zero:
    OS << "zero";
    return;
  case InstructionExprFactory::Kind::One:
    OS << "one";
    return;
  case InstructionExprFactory::Kind::Atom:
    OS << "atom("
       << formatTransfer(Expr->Transfer ? *Expr->Transfer : nullptr, ValueToId)
       << ")";
    return;
  case InstructionExprFactory::Kind::Union:
    OS << "union(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ",";
    formatPathExpr(OS, Expr->R, ValueToId);
    OS << ")";
    return;
  case InstructionExprFactory::Kind::Concat:
    OS << "concat(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ",";
    formatPathExpr(OS, Expr->R, ValueToId);
    OS << ")";
    return;
  case InstructionExprFactory::Kind::Star:
    OS << "star(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ")";
    return;
  }
}

template <typename ResultT>
void printSolveMetadata(raw_ostream &OS, const ResultT &Result) {
  if (!Result.hasSolveMetadata())
    return;
  const auto &Diag = Result.solveDiagnostics();
  OS << "  [solver] status=" << toString(Result.solveStatus())
     << ", requested=" << toString(Diag.requested_method)
     << ", executed=" << toString(Diag.executed_method)
     << ", used_adt=" << (Diag.used_adt ? "true" : "false")
     << ", fallback=" << toString(Diag.fallback_reason)
     << ", star_iters=" << Diag.star_iterations_total
     << ", max_star_hit=" << (Diag.max_star_hit ? "true" : "false")
     << ", peak_nodes=" << Diag.peak_matrix_nodes << "\n";
}

template <unsigned K, typename FactT, typename TransferT, typename NodeT>
void printSolveMetadata(
    raw_ostream &OS,
    const elimination::InterDataFlowResultT<K, FactT, TransferT, NodeT>
        &Result) {
  if (!Result.hasSolveMetadata())
    return;
  OS << "  [solver] status=" << toString(Result.solveStatus()) << "\n";
}

template <typename ResultT>
void dumpProfile(raw_ostream &OS, const FunctionView &View,
                 const ResultT &Result, const Timings &T) {
  const auto CFG = collectCFGStats(View.Function);
  OS << "  [cfg] args=" << CFG.Arguments << ", blocks=" << CFG.Blocks
     << ", insts=" << CFG.Instructions << ", edges=" << CFG.Edges
     << ", branching_blocks=" << CFG.BranchingBlocks
     << ", max_succs=" << CFG.MaxSuccessors << ", phis=" << CFG.PhiNodes
     << ", calls=" << CFG.Calls << ", returns=" << CFG.Returns
     << ", unreachable=" << CFG.Unreachable
     << ", elapsed_us=" << T.end2end << "\n";
  OS << "  [timing] gen_us=" << T.gen << ", norm_us=" << T.norm
     << ", interp_us=" << T.interp << ", end2end_us=" << T.end2end
     << ", runs=" << T.runs << "\n";
  printSolveMetadata(OS, Result);

  // Unified DAG statistics over the whole summary batch (matches the synthetic
  // evaluation's DagStats — the Table VI structural metrics).
  std::vector<InstructionExprRef> Batch;
  Batch.reserve(View.OrderedInsts.size());
  for (auto *I : View.OrderedInsts) {
    auto E = Result.ExprTo(I);
    if (E)
      Batch.push_back(E);
  }
  const auto DS = elimination::ean::computeDagStats<Instruction *>(Batch);
  OS << "  [dagstats] nodes=" << DS.uniqueNodes << ", edges=" << DS.uniqueEdges
     << ", tree=" << DS.expandedTree << ", seq=" << DS.concats
     << ", stars=" << DS.stars << ", unions=" << DS.unions
     << ", atoms=" << DS.atoms << ", sharing=" << DS.sharing()
     << ", roots=" << Batch.size() << "\n";

  size_t NodesWithExpr = 0;
  size_t MissingExpr = 0;
  size_t TotalUniqueNodes = 0;
  size_t TotalSharedRefs = 0;
  size_t TotalStars = 0;
  size_t TotalUnions = 0;
  size_t TotalConcats = 0;
  size_t MaxExprNodes = 0;
  size_t MaxExprDepth = 0;
  std::string MaxExprInst = "none";
  std::string DeepestExprInst = "none";

  for (auto *I : View.OrderedInsts) {
    const auto Expr = Result.ExprTo(I);
    if (!Expr) {
      ++MissingExpr;
      continue;
    }
    ++NodesWithExpr;
    const auto Profile = collectExprProfile(Expr);
    TotalUniqueNodes += Profile.UniqueNodes;
    TotalSharedRefs += Profile.SharedRefs;
    TotalStars += Profile.StarNodes;
    TotalUnions += Profile.UnionNodes;
    TotalConcats += Profile.ConcatNodes;
    if (Profile.UniqueNodes > MaxExprNodes) {
      MaxExprNodes = Profile.UniqueNodes;
      MaxExprInst = View.ValueToId.at(I);
    }
    if (Profile.MaxDepth > MaxExprDepth) {
      MaxExprDepth = Profile.MaxDepth;
      DeepestExprInst = View.ValueToId.at(I);
    }
  }

  OS << "  [expr-profile] with_expr=" << NodesWithExpr
     << ", missing_expr=" << MissingExpr
     << ", total_unique_nodes=" << TotalUniqueNodes
     << ", total_shared_refs=" << TotalSharedRefs
     << ", total_unions=" << TotalUnions << ", total_concats=" << TotalConcats
     << ", total_stars=" << TotalStars << ", max_nodes=" << MaxExprNodes << "@"
     << MaxExprInst << ", max_depth=" << MaxExprDepth << "@" << DeepestExprInst
     << "\n";

  if (!DumpExprsOpt)
    return;

  for (auto *I : View.OrderedInsts) {
    const auto Expr = Result.ExprTo(I);
    OS << "  [expr] " << View.ValueToId.at(I);
    if (!Expr) {
      OS << " missing\n";
      continue;
    }
    const auto Profile = collectExprProfile(Expr);
    OS << " nodes=" << Profile.UniqueNodes << ", depth=" << Profile.MaxDepth
       << ", atoms=" << Profile.AtomNodes << ", unions=" << Profile.UnionNodes
       << ", concats=" << Profile.ConcatNodes << ", stars=" << Profile.StarNodes
       << ", shared_refs=" << Profile.SharedRefs << ", expr=";
    formatPathExpr(OS, Expr, View.ValueToId);
    OS << "\n";
  }
}

template <typename ResultT, typename Printer>
void dumpTimedResult(raw_ostream &OS, const FunctionView &View, ResultT &Result,
                     const Timings &T, Printer &&PrintState) {
  if (DumpProfileOpt || DumpExprsOpt)
    dumpProfile(OS, View, Result, T);
  lotus::dataflow_tool::printInstructionStates(
      OS, View, [&](Instruction *I) { PrintState(I, Result); });
}

template <typename Runner, typename Printer>
void runTimedAnalysis(raw_ostream &OS, const FunctionView &View,
                      const elimination::EliminationOptions &ElimOpts,
                      Runner &&Run, Printer &&PrintState) {
  for (unsigned W = 0; W < WarmupOpt; ++W) {
    auto Warm = Run(View.Function, ElimOpts);
    (void)Warm;
  }
  const unsigned R = std::max(1u, static_cast<unsigned>(RepeatOpt));
  std::vector<std::uint64_t> Gen, Norm, Interp, End;
  Gen.reserve(R);
  Norm.reserve(R);
  Interp.reserve(R);
  End.reserve(R);

  // The R-1 timing-only runs are constructed and discarded (DataFlowResultT is
  // not assignable, so we never reassign — we construct fresh each run).
  auto Sample = [&](const auto &Res, std::uint64_t Us) {
    const auto &D = Res.solveDiagnostics();
    Gen.push_back(D.gen_time_us);
    Norm.push_back(D.norm_time_us);
    Interp.push_back(D.interp_time_us);
    End.push_back(Us);
  };
  for (unsigned I = 0; I + 1 < R; ++I) {
    const auto Start = std::chrono::steady_clock::now();
    auto Tmp = Run(View.Function, ElimOpts);
    Sample(Tmp, static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - Start)
                        .count()));
  }
  // Final measured run is kept for the structural dump.
  const auto Start = std::chrono::steady_clock::now();
  auto Result = Run(View.Function, ElimOpts);
  Sample(Result, static_cast<std::uint64_t>(
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - Start)
                         .count()));

  const Timings T{medianOf(Gen), medianOf(Norm), medianOf(Interp),
                  medianOf(End), R};
  dumpTimedResult(OS, View, Result, T, std::forward<Printer>(PrintState));
}

template <typename ResultT, typename Printer>
void dumpInterproceduralResult(raw_ostream &OS, Module &M,
                               const ValueIdMap &ValueToId,
                               const ResultT &Result, Printer &&PrintState) {
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    lotus::dataflow_tool::emitFunctionHeader(OS, F);
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto Contexts = Result.contextsForInstruction(&I);
        std::sort(Contexts.begin(), Contexts.end(),
                  [&](const auto &Lhs, const auto &Rhs) {
                    return contextToString(Lhs.Ctx) < contextToString(Rhs.Ctx);
                  });
        if (Contexts.empty()) {
          OS << "  " << ValueToId.at(&I) << " IN: <no-context>\n";
          continue;
        }
        for (const auto &Key : Contexts) {
          OS << "  " << ValueToId.at(&I) << " IN [" << contextToString(Key.Ctx)
             << "]: ";
          PrintState(Key, Result);
          OS << "\n";
        }
      }
    }
  }
}

template <typename Runner, typename Printer>
void runTimedInterproceduralAnalysis(raw_ostream &OS, Module &M,
                                     Function &Entry, Runner &&Run,
                                     Printer &&PrintState) {
  const auto Start = std::chrono::steady_clock::now();
  auto Result = Run(Entry);
  const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - Start);
  const auto ValueToId = buildModuleValueIdMap(M);
  OS << "  [profile] elapsed_us=" << Elapsed.count() << "\n";
  printSolveMetadata(OS, Result);
  dumpInterproceduralResult(OS, M, ValueToId, Result,
                            [&](const auto &Key, const auto &Res) {
                              PrintState(Key, Res, ValueToId);
                            });
}

template <typename Runner>
void runSetIntraAnalysis(raw_ostream &OS, const FunctionView &View,
                         const elimination::EliminationOptions &ElimOpts,
                         Runner &&Run) {
  runTimedAnalysis(OS, View, ElimOpts, std::forward<Runner>(Run),
                   [&](Instruction *I, auto &Result) {
                     lotus::dataflow_tool::formatValueSet(OS, Result.IN(I),
                                                          View.ValueToId);
                   });
}

template <typename Runner>
void runBoolIntraAnalysis(raw_ostream &OS, const FunctionView &View,
                          const elimination::EliminationOptions &ElimOpts,
                          Runner &&Run) {
  runTimedAnalysis(OS, View, ElimOpts, std::forward<Runner>(Run),
                   [&](Instruction *I, auto &Result) {
                     OS << (Result.IN(I) ? "true" : "false");
                   });
}

template <typename Runner>
void runSetInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                         Runner &&Run) {
  runTimedInterproceduralAnalysis(
      OS, M, Entry, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

template <typename Runner, typename Formatter>
void runMapInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                         Runner &&Run, Formatter &&FormatValue) {
  runTimedInterproceduralAnalysis(
      OS, M, Entry, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueMap(OS, Result.IN(Key), ValueToId,
                                             FormatValue);
      });
}

template <typename Runner>
void runBoolInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                          Runner &&Run) {
  runTimedInterproceduralAnalysis(
      OS, M, Entry, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto & /*ValueToId*/) {
        OS << (Result.IN(Key) ? "true" : "false");
      });
}

void runLiveness(raw_ostream &OS, const FunctionView &View,
                 const elimination::EliminationOptions &ElimOpts) {
  runSetIntraAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimLiveVariables(&F, Opts);
      });
}

void runReachingDefinitions(raw_ostream &OS, const FunctionView &View,
                            const elimination::EliminationOptions &ElimOpts) {
  runSetIntraAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimReachingDefinitions(&F, nullptr, Opts);
      });
}

void runUninitialized(raw_ostream &OS, const FunctionView &View,
                      const elimination::EliminationOptions &ElimOpts) {
  runSetIntraAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimUninitVariables(&F, nullptr, Opts);
      });
}

void runConstantPropagation(raw_ostream &OS, const FunctionView &View,
                            const elimination::EliminationOptions &ElimOpts) {
  runTimedAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimConstantPropagation(&F, nullptr, Opts);
      },
      [&](Instruction *I, auto &Result) {
        lotus::dataflow_tool::formatValueMap(
            OS, Result.IN(I), View.ValueToId,
            [&](const ValueLatticeElement &Value) {
              return formatValueLatticeElement(Value);
            });
      });
}

void runAvailableExpressions(raw_ostream &OS, const FunctionView &View,
                             const elimination::EliminationOptions &ElimOpts) {
  runTimedAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimAvailableExpressions(&F, nullptr, Opts);
      },
      [&](Instruction *I, auto &Result) {
        std::vector<std::string> Exprs;
        for (const auto &Expr : Result.IN(I))
          Exprs.push_back(formatExpressionKey(Expr));
        std::sort(Exprs.begin(), Exprs.end());
        for (size_t Index = 0; Index < Exprs.size(); ++Index) {
          if (Index)
            OS << ",";
          OS << Exprs[Index];
        }
      });
}

void runReachable(raw_ostream &OS, const FunctionView &View,
                  const elimination::EliminationOptions &ElimOpts) {
  runBoolIntraAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimReachable(&F, Opts);
      });
}

void runInterLiveness(raw_ostream &OS, Module &M, Function &Entry) {
  runSetInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimLiveVariables(&F);
  });
}

void runInterReachingDefinitions(raw_ostream &OS, Module &M, Function &Entry) {
  runSetInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimReachingDefinitions(&F);
  });
}

void runInterUninitialized(raw_ostream &OS, Module &M, Function &Entry) {
  runSetInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimUninitVariables(&F);
  });
}

void runInterConstantPropagation(raw_ostream &OS, Module &M, Function &Entry) {
  runMapInterAnalysis(
      OS, M, Entry,
      [](Function &F) {
        return elimination::runInterElimConstantPropagation(&F);
      },
      [&](const ValueLatticeElement &Value) {
        return formatValueLatticeElement(Value);
      });
}

void runInterReachable(raw_ostream &OS, Module &M, Function &Entry) {
  runBoolInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimReachable(&F);
  });
}

struct AnalysisHandler final {
  StringRef Name;
  bool ModuleScoped = false;
  void (*RunFunction)(raw_ostream &, const FunctionView &,
                      const elimination::EliminationOptions &) = nullptr;
  void (*RunModule)(raw_ostream &, Module &, Function &) = nullptr;
};

const AnalysisHandler Handlers[] = {
    {"liveness", false, &runLiveness, nullptr},
    {"reaching_defs", false, &runReachingDefinitions, nullptr},
    {"uninitialized", false, &runUninitialized, nullptr},
    {"constant_prop", false, &runConstantPropagation, nullptr},
    {"available_exprs", false, &runAvailableExpressions, nullptr},
    {"reachable", false, &runReachable, nullptr},
    {"inter_liveness", true, nullptr, &runInterLiveness},
    {"inter_reaching_defs", true, nullptr, &runInterReachingDefinitions},
    {"inter_uninitialized", true, nullptr, &runInterUninitialized},
    {"inter_constant_prop", true, nullptr, &runInterConstantPropagation},
    {"inter_reachable", true, nullptr, &runInterReachable},
};

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Elimination engine testing\n");

  LLVMContext Context;
  SMDiagnostic Err;
  auto M = lotus::dataflow_tool::loadModuleOrReport(InputFilename, Context, Err,
                                                    argv[0]);
  if (!M)
    return 1;

  lotus::dataflow_tool::prepareModule(*M);

  raw_null_ostream NullOS;
  std::unique_ptr<raw_fd_ostream> FileOS;
  std::error_code EC;
  raw_ostream &OS = lotus::dataflow_tool::selectOutputStream(
      StdoutOpt, OutDir, "elim.txt", FileOS, NullOS, EC);
  if (EC) {
    errs() << "error: cannot create " << OutDir << "/elim.txt: " << EC.message()
           << "\n";
    return 1;
  }

  // Parse a comma-separated client list (amortizes module loading across
  // clients in one process).
  std::vector<const AnalysisHandler *> Clients;
  {
    std::stringstream SS(AnalysisOpt);
    std::string Name;
    while (std::getline(SS, Name, ',')) {
      if (Name.empty())
        continue;
      const auto *H = lotus::dataflow_tool::findHandler(StringRef(Name), Handlers);
      if (!H) {
        errs() << "error: unknown elimination analysis '" << Name << "'\n";
        return 1;
      }
      Clients.push_back(H);
    }
  }
  if (Clients.empty()) {
    errs() << "error: no analysis selected\n";
    return 1;
  }

  const auto ElimOpts = buildElimOpts();
  OS << "[elim] clients=" << AnalysisOpt << ", method=" << ElimMethodOpt
     << ", ordering=" << OrderingOpt << ", ean=" << (EanOpt ? "on" : "off")
     << ", ean_laws=" << EanLawsOpt << ", repeat=" << RepeatOpt
     << ", max_func_insts=" << MaxFuncInsts << "\n";

  const bool AnyModule =
      std::any_of(Clients.begin(), Clients.end(),
                  [](const AnalysisHandler *H) { return H->ModuleScoped; });
  if (AnyModule) {
    if (Clients.size() != 1) {
      errs() << "error: module-scoped analyses must be run one at a time\n";
      return 1;
    }
    Function *Entry = M->getFunction(EntryFunctionOpt);
    if (Entry == nullptr || Entry->isDeclaration()) {
      errs() << "error: entry function '" << EntryFunctionOpt
             << "' not found or is a declaration\n";
      return 1;
    }
    Clients.front()->RunModule(OS, *M, *Entry);
  } else {
    std::size_t Skipped = 0;
    lotus::dataflow_tool::forEachDefinedFunction(
        *M, OS, [&](const FunctionView &View) {
          if (MaxFuncInsts != 0 &&
              View.OrderedInsts.size() > MaxFuncInsts) {
            OS << "  [skipped] reason=too_large insts="
               << View.OrderedInsts.size() << "\n";
            ++Skipped;
            return;
          }
          for (const AnalysisHandler *H : Clients) {
            OS << "  [client:" << H->Name << "]\n";
            H->RunFunction(OS, View, ElimOpts);
          }
        });
    OS << "[summary] skipped_functions=" << Skipped << "\n";
  }

  OS << "[mem] peak_rss_kb=" << peakRssKb() << "\n";
  return 0;
}

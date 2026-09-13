#include "Dataflow/APA/Analyses/Intra/Reachability.h"

#include "Dataflow/APA/Baseline/TranslAPA/Driver.h"

namespace elimination {
namespace {

class ElimReachableProblem : public LLVMIntraEliminationProblem<ReachableFact, ReachabilityDomain> {
public:
  explicit ElimReachableProblem(llvm::Function *F)
      : LLVMIntraEliminationProblem<ReachableFact, ReachabilityDomain>(F) {}

  ReachableFact applyTransfer(const transfer_t & /*T*/,
                              const ReachableFact &In) const override {
    return In;
  }

  ReachableFact initialFact() const override { return true; }
};

} // namespace

ReachableResult runIntraElimReachable(llvm::Function *F,
                                      EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return ReachableResult{};
  }

  ElimReachableProblem Problem(F);
  IntraEliminationSolver<LLVMAnalysisTypes<ReachableFact, ReachabilityDomain>> Solver(Problem,
                                                                      Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  Out.setSolveMetadata(Status, Solver.getDiagnostics());
  return Out;
}

namespace {
// One-fact translator for the degenerate reachability lattice: the universe is
// a single "reachable" bit and every atom is the identity transfer ({}, {}).
struct ReachableTranslator {
  unsigned universeSize() const { return 1; }
  translapa::GenKillFact translate(llvm::Instruction *const & /*T*/) const {
    return {llvm::BitVector(1, false), llvm::BitVector(1, false)};
  }
  llvm::BitVector toBits(const ReachableFact &R) const {
    return llvm::BitVector(1, R);
  }
  ReachableFact fromBits(const llvm::BitVector &B) const { return B.test(0); }
};
} // namespace

ReachableResult runIntraTranslApaReachable(llvm::Function *F,
                                           EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return ReachableResult{};
  }

  // See IntraReachingDefinitions.cpp for the rationale: under EAN/Greedy the
  // post-pass rewrites Results.ExprTo in place, so InterpMemo lets us fold the
  // optimized DAG instead of paying a discarded generic eval.
  if (Opts.EnableEAN || Opts.EnableGreedy) {
    Opts.InterpMemo = true;
  }

  ElimReachableProblem Problem(F);
  IntraEliminationSolver<LLVMAnalysisTypes<ReachableFact, ReachabilityDomain>>
      Solver(Problem, Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  auto Diag = Solver.getDiagnostics();

  ReachableTranslator Tr;
  auto TT =
      translapa::foldFillGenKillTimed<
          LLVMAnalysisTypes<ReachableFact, ReachabilityDomain>>(Problem, Out,
                                                                Tr);
  Diag.norm_time_us += TT.extract_us;
  Diag.interp_time_us = TT.fold_us;
  Out.setSolveMetadata(Status, Diag);
  return Out;
}

} // namespace elimination

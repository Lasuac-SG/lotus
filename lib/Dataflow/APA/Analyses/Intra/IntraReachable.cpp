#include "Dataflow/APA/Analyses/Intra/IntraReachability.h"

#include "Dataflow/APA/Baseline/TranslAPA/Driver.h"

namespace elimination {
namespace {

class ElimReachableProblem : public LLVMIntraEliminationProblem<ReachableFact> {
public:
  explicit ElimReachableProblem(llvm::Function *F)
      : LLVMIntraEliminationProblem<ReachableFact>(F) {}

  ReachableFact applyTransfer(const transfer_t & /*T*/,
                              const ReachableFact &In) const override {
    return In;
  }

  ReachableFact meet(const ReachableFact &Lhs,
                     const ReachableFact &Rhs) const override {
    return Lhs || Rhs;
  }

  bool equal_to(const ReachableFact &Lhs,
                const ReachableFact &Rhs) const override {
    return Lhs == Rhs;
  }

  ReachableFact meetIdentity() const override { return false; }

  ReachableFact initialFact() const override { return true; }
};

} // namespace

ReachableResult runIntraElimReachable(llvm::Function *F,
                                      EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return ReachableResult{};
  }

  ElimReachableProblem Problem(F);
  IntraEliminationSolver<LLVMEliminationDomain<ReachableFact>> Solver(Problem,
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

  ElimReachableProblem Problem(F);
  IntraEliminationSolver<LLVMEliminationDomain<ReachableFact>> Solver(Problem,
                                                                      Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  auto Diag = Solver.getDiagnostics();

  ReachableTranslator Tr;
  auto TT =
      translapa::foldFillGenKillTimed<LLVMEliminationDomain<ReachableFact>>(
          Problem, Out, Tr);
  Diag.norm_time_us = TT.extract_us;
  Diag.interp_time_us = TT.fold_us;
  Out.setSolveMetadata(Status, Diag);
  return Out;
}

} // namespace elimination

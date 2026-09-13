#include "Dataflow/APA/Analyses/Intra/ReachingDefinitions.h"

#include "Dataflow/APA/Baseline/TranslAPA/AtomTranslator.h"
#include "Dataflow/APA/Baseline/TranslAPA/Driver.h"

#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/IR/Instructions.h"

#include <unordered_map>

namespace elimination {
namespace {

class ElimReachingDefinitionsProblem
    : public LLVMIntraEliminationProblem<ReachingDefinitionsFact, ReachingDefinitionsDomain> {
public:
  explicit ElimReachingDefinitionsProblem(llvm::Function *F,
                                          llvm::AAResults *AA = nullptr,
                                          llvm::MemorySSA *MSSA = nullptr)
      : LLVMIntraEliminationProblem<ReachingDefinitionsFact, ReachingDefinitionsDomain>(F), AA(AA),
        MSSA(MSSA) {
    buildTransferCache(F);
  }

  ReachingDefinitionsFact
  applyTransfer(const transfer_t &T,
                const ReachingDefinitionsFact &In) const override {
    ReachingDefinitionsFact Out = In;
    auto It = Transfers.find(T);
    if (It == Transfers.end()) {
      return Out;
    }
    Out.subtract(It->second.Kill);
    Out.unionWith(It->second.Gen);
    return Out;
  }

  ReachingDefinitionsFact initialFact() const override {
    ReachingDefinitionsFact Out = this->bottom();
    auto *F = this->entry() != nullptr ? this->entry()->getFunction() : nullptr;
    if (F == nullptr) {
      return Out;
    }
    for (auto &Arg : F->args()) {
      Out.insert(&Arg);
    }
    return Out;
  }

private:
  llvm::AAResults *AA = nullptr;
  llvm::MemorySSA *MSSA = nullptr;
  struct TransferInfo {
    ReachingDefinitionsFact Gen;
    ReachingDefinitionsFact Kill;
  };
  std::unordered_map<llvm::Instruction *, TransferInfo> Transfers;

  void buildTransferCache(llvm::Function *F) {
    if (F == nullptr || F->isDeclaration())
      return;

    auto AllFacts = this->bottom();
    for (auto &Arg : F->args())
      AllFacts.insert(&Arg);
    for (auto &BB : *F)
      for (auto &I : BB)
        if (llvm::isa<llvm::StoreInst>(&I) || !I.getType()->isVoidTy())
          AllFacts.insert(&I);

    for (auto &BB : *F) {
      for (auto &I : BB) {
        auto &Info = Transfers[&I];
        Info.Gen = this->bottom();
        Info.Kill = this->bottom();
        auto Survivors = AllFacts;
        bool HasKill = false;

        if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
          HasKill = true;
          if (MSSA != nullptr)
            killStoresWithMemorySSA(Store, Survivors);
          else if (AA == nullptr)
            killAllStores(Survivors);
          else
            killAliasedStores(Store, Survivors);
          Info.Gen.insert(Store);
        } else if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
          if (Call->mayWriteToMemory()) {
            HasKill = true;
            if (MSSA != nullptr)
              killStoresWithMemorySSA(Call, Survivors);
            else if (AA == nullptr)
              killAllStores(Survivors);
            else
              killStoresModdedByCall(Call, Survivors);
          }
        }

        if (!I.getType()->isVoidTy())
          Info.Gen.insert(&I);
        if (HasKill) {
          Info.Kill = AllFacts;
          Info.Kill.subtract(Survivors);
        }
      }
    }
  }

  static void killAllStores(ReachingDefinitionsFact &Out) {
    for (auto It = Out.begin(); It != Out.end();) {
      if (llvm::isa<llvm::StoreInst>(*It)) {
        It = Out.erase(It);
      } else {
        ++It;
      }
    }
  }

  void killAliasedStores(const llvm::StoreInst *Store,
                         ReachingDefinitionsFact &Out) const {
    if (AA == nullptr || Store == nullptr) {
      killAllStores(Out);
      return;
    }
    auto StoreLoc = llvm::MemoryLocation::get(Store);
    for (auto It = Out.begin(); It != Out.end();) {
      auto *Def = llvm::dyn_cast<llvm::StoreInst>(*It);
      if (Def == nullptr) {
        ++It;
        continue;
      }
      auto DefLoc = llvm::MemoryLocation::get(Def);
      if (AA->alias(StoreLoc, DefLoc) != llvm::AliasResult::NoAlias) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }

  void killStoresModdedByCall(const llvm::CallBase *Call,
                              ReachingDefinitionsFact &Out) const {
    if (AA == nullptr || Call == nullptr) {
      killAllStores(Out);
      return;
    }
    for (auto It = Out.begin(); It != Out.end();) {
      auto *Def = llvm::dyn_cast<llvm::StoreInst>(*It);
      if (Def == nullptr) {
        ++It;
        continue;
      }
      auto DefLoc = llvm::MemoryLocation::get(Def);
      auto Info = AA->getModRefInfo(Call, DefLoc);
      if (llvm::isModSet(Info)) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }

  void killStoresWithMemorySSA(const llvm::Instruction *Inst,
                               ReachingDefinitionsFact &Out) const {
    if (MSSA == nullptr || Inst == nullptr) {
      killAllStores(Out);
      return;
    }
    auto *MA = MSSA->getMemoryAccess(Inst);
    if (MA == nullptr) {
      killAllStores(Out);
      return;
    }
    auto *Walker = MSSA->getWalker();
    for (auto It = Out.begin(); It != Out.end();) {
      auto *Def = llvm::dyn_cast<llvm::StoreInst>(*It);
      if (Def == nullptr) {
        ++It;
        continue;
      }
      auto DefLoc = llvm::MemoryLocation::get(Def);
      auto *Clobber = Walker->getClobberingMemoryAccess(MA, DefLoc);
      if (Clobber == MA) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }
};

} // namespace

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, EliminationOptions Opts) {
  return runIntraElimReachingDefinitions(F, nullptr, Opts);
}

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, llvm::AAResults *AA,
                                EliminationOptions Opts) {
  return runIntraElimReachingDefinitions(F, AA, nullptr, Opts);
}

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, llvm::AAResults *AA,
                                llvm::MemorySSA *MSSA,
                                EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return ReachingDefinitionsResult{};
  }

  ElimReachingDefinitionsProblem Problem(F, AA, MSSA);
  IntraEliminationSolver<LLVMAnalysisTypes<ReachingDefinitionsFact, ReachingDefinitionsDomain>> Solver(
      Problem, Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  Out.setSolveMetadata(Status, Solver.getDiagnostics());
  return Out;
}

ReachingDefinitionsResult
runIntraTranslApaReachingDefinitions(llvm::Function *F, llvm::AAResults *AA,
                                     EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return ReachingDefinitionsResult{};
  }

  // Composition (EAN/Greedy ⊕ TranslAPA): those post-passes rewrite
  // Results.ExprTo(N) to the optimized DAG in place. Enabling InterpMemo makes
  // the solver's post-pass (and the base materialization) skip their generic
  // eval, since we fill IN by folding the (now optimized) DAG ourselves. Without
  // this the run would waste — and possibly time out on — a full generic
  // interpretation of the graph we are about to fold instead. This is what turns
  // "--ean --interp=translapa" into a genuine measurement of folding the
  // EAN-reduced DAG. The EAN/Greedy normalization time is already accumulated in
  // norm_time_us by the post-pass; we add the mechanical extraction below.
  if (Opts.EnableEAN || Opts.EnableGreedy) {
    Opts.InterpMemo = true;
  }

  ElimReachingDefinitionsProblem Problem(F, AA, nullptr);
  IntraEliminationSolver<
      LLVMAnalysisTypes<ReachingDefinitionsFact, ReachingDefinitionsDomain>>
      Solver(Problem, Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  auto Diag = Solver.getDiagnostics();

  // The finite fact universe: every value the transfer can put in a fact set —
  // function arguments (initial fact), store instructions, and non-void
  // instructions (see applyTransfer above).
  std::vector<const llvm::Value *> Universe;
  for (auto &Arg : F->args()) {
    Universe.push_back(&Arg);
  }
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (llvm::isa<llvm::StoreInst>(&I)) {
        Universe.push_back(&I);
      } else if (!I.getType()->isVoidTy()) {
        Universe.push_back(&I);
      }
    }
  }

  translapa::GenKillAtomTranslator<llvm::Instruction *, ReachingDefinitionsFact>
      Tr(std::move(Universe),
         [&Problem](const llvm::Instruction *const &T,
                    const ReachingDefinitionsFact &In) {
           return Problem.applyTransfer(const_cast<llvm::Instruction *>(T), In);
         },
         /*Separable=*/true);

  auto TT =
      translapa::foldFillGenKillTimed<LLVMAnalysisTypes<
          ReachingDefinitionsFact, ReachingDefinitionsDomain>>(Problem, Out, Tr);
  // Extraction is the one-time IDA->APA translation cost (paper's construction);
  // the closed-form fold is the per-query evaluation cost. Under EAN/Greedy the
  // post-pass has already put its saturation time into norm_time_us, so we add
  // (not overwrite) the extraction to report total construction = EAN + extract.
  Diag.norm_time_us += TT.extract_us;
  Diag.interp_time_us = TT.fold_us;
  Out.setSolveMetadata(Status, Diag);
  return Out;
}

} // namespace elimination

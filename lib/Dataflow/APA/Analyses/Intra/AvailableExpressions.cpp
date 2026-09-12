#include "Dataflow/APA/Analyses/Intra/AvailableExpressions.h"

#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <iterator>
#include <unordered_map>

namespace elimination {
namespace {

bool isCandidateExpr(const llvm::Instruction *Inst,
                     const llvm::DominatorTree *DT,
                     const llvm::TargetLibraryInfo *TLI) {
  if (Inst == nullptr || Inst->getType()->isVoidTy()) {
    return false;
  }
  if (llvm::isa<llvm::PHINode>(Inst) || llvm::isa<llvm::AllocaInst>(Inst) ||
      Inst->isTerminator()) {
    return false;
  }
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
    return !Load->isVolatile();
  }
  if (Inst->mayHaveSideEffects()) {
    return false;
  }
  return llvm::isSafeToSpeculativelyExecute(Inst, Inst, DT, TLI);
}

ExpressionKey makeKeyWithMSSA(const llvm::Instruction *Inst,
                              llvm::MemorySSA *MSSA) {
  auto Key = makeExpressionKey(Inst);
  if (MSSA == nullptr || Inst == nullptr) {
    return Key;
  }
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
    auto *Walker = MSSA->getWalker();
    auto *Clobber = Walker->getClobberingMemoryAccess(Load);
    Key.MemoryAccess = Clobber;
  }
  return Key;
}

class ElimAvailableExpressionsProblem
    : public LLVMIntraEliminationProblem<AvailableExpressionsFact, AvailableExpressionsDomain> {
public:
  explicit ElimAvailableExpressionsProblem(
      llvm::Function *F, llvm::AAResults *AA = nullptr,
      llvm::DominatorTree *DT = nullptr, llvm::TargetLibraryInfo *TLI = nullptr,
      llvm::MemorySSA *MSSA = nullptr)
      : LLVMIntraEliminationProblem<AvailableExpressionsFact, AvailableExpressionsDomain>(F), AA(AA),
        DT(DT), TLI(TLI), MSSA(MSSA) {
    buildUniverse(F);
    this->getAbstractDomain().setUniverse(AllExprs);
    buildTransferCache(F);
  }

  AvailableExpressionsFact
  applyTransfer(const transfer_t &T,
                const AvailableExpressionsFact &In) const override {
    AvailableExpressionsFact Out = In;
    auto It = Transfers.find(T);
    if (It == Transfers.end()) {
      return Out;
    }
    Out.subtract(It->second.Kill);
    Out.unionWith(It->second.Gen);
    return Out;
  }

  AvailableExpressionsFact initialFact() const override {
    return this->getAbstractDomain().empty();
  }

private:
  llvm::AAResults *AA = nullptr;
  llvm::DominatorTree *DT = nullptr;
  llvm::TargetLibraryInfo *TLI = nullptr;
  llvm::MemorySSA *MSSA = nullptr;
  AvailableExpressionsFact AllExprs;
  std::set<ExpressionKey> LoadExprs;
  struct TransferInfo {
    AvailableExpressionsFact Gen;
    AvailableExpressionsFact Kill;
  };
  std::unordered_map<llvm::Instruction *, TransferInfo> Transfers;

  void buildUniverse(llvm::Function *F) {
    AllExprs.clear();
    LoadExprs.clear();
    if (F == nullptr || F->isDeclaration()) {
      return;
    }
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (!isCandidateExpr(&I, DT, TLI)) {
          continue;
        }
        auto Key = makeKeyWithMSSA(&I, MSSA);
        AllExprs.insert(Key);
        if (llvm::isa<llvm::LoadInst>(&I)) {
          LoadExprs.insert(Key);
        }
      }
    }
  }

  void buildTransferCache(llvm::Function *F) {
    if (F == nullptr || F->isDeclaration())
      return;
    for (auto &BB : *F) {
      for (auto &I : BB) {
        auto &Info = Transfers[&I];
        Info.Gen = this->getAbstractDomain().empty();
        Info.Kill = this->getAbstractDomain().empty();

        if (killsMemory(&I)) {
          auto Survivors = AllExprs;
          if (MSSA != nullptr)
            killLoadsWithMemorySSA(&I, Survivors);
          else if (AA == nullptr)
            killAllLoads(Survivors);
          else
            killAliasedLoads(&I, Survivors);
          Info.Kill = AllExprs;
          Info.Kill.subtract(Survivors);
        }

        if (isCandidateExpr(&I, DT, TLI))
          Info.Gen.insert(makeKeyWithMSSA(&I, MSSA));
      }
    }
  }

  static bool killsMemory(const llvm::Instruction *Inst) {
    if (Inst == nullptr) {
      return false;
    }
    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
      return Load->isVolatile();
    }
    return Inst->mayWriteToMemory();
  }

  void killAllLoads(AvailableExpressionsFact &Out) const {
    for (auto It = Out.begin(); It != Out.end();) {
      if (LoadExprs.count(*It)) {
        It = Out.erase(It);
      } else {
        ++It;
      }
    }
  }

  void killAliasedLoads(const llvm::Instruction *Inst,
                        AvailableExpressionsFact &Out) const {
    if (AA == nullptr || Inst == nullptr) {
      killAllLoads(Out);
      return;
    }

    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
      auto StoreLoc = llvm::MemoryLocation::get(Store);
      for (auto It = Out.begin(); It != Out.end();) {
        const auto &Key = *It;
        if (!isLoadKey(Key)) {
          ++It;
          continue;
        }
        auto *Ptr = getLoadPointerOperand(Key);
        if (Ptr == nullptr) {
          It = Out.erase(It);
          continue;
        }
        auto LoadLoc = llvm::MemoryLocation(
            Ptr, llvm::LocationSize::beforeOrAfterPointer(), Key.AATags);
        if (AA->alias(StoreLoc, LoadLoc) != llvm::AliasResult::NoAlias) {
          It = Out.erase(It);
          continue;
        }
        ++It;
      }
      return;
    }

    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      if (!Call->mayWriteToMemory()) {
        return;
      }
      for (auto It = Out.begin(); It != Out.end();) {
        const auto &Key = *It;
        if (!isLoadKey(Key)) {
          ++It;
          continue;
        }
        auto *Ptr = getLoadPointerOperand(Key);
        if (Ptr == nullptr) {
          It = Out.erase(It);
          continue;
        }
        auto LoadLoc = llvm::MemoryLocation(
            Ptr, llvm::LocationSize::beforeOrAfterPointer(), Key.AATags);
        auto Info = AA->getModRefInfo(Call, LoadLoc);
        if (llvm::isModSet(Info)) {
          It = Out.erase(It);
          continue;
        }
        ++It;
      }
      return;
    }

    killAllLoads(Out);
  }

  void killLoadsWithMemorySSA(const llvm::Instruction *Inst,
                              AvailableExpressionsFact &Out) const {
    if (MSSA == nullptr || Inst == nullptr) {
      killAllLoads(Out);
      return;
    }
    auto *MA = MSSA->getMemoryAccess(Inst);
    if (MA == nullptr) {
      killAllLoads(Out);
      return;
    }
    auto *Walker = MSSA->getWalker();
    for (auto It = Out.begin(); It != Out.end();) {
      const auto &Key = *It;
      if (!isLoadKey(Key)) {
        ++It;
        continue;
      }
      auto *Ptr = getLoadPointerOperand(Key);
      if (Ptr == nullptr) {
        It = Out.erase(It);
        continue;
      }
      llvm::MemoryLocation LoadLoc(
          Ptr, llvm::LocationSize::beforeOrAfterPointer(), Key.AATags);
      auto *Clobber = Walker->getClobberingMemoryAccess(MA, LoadLoc);
      if (Clobber == MA) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }
};

} // namespace

AvailableExpressionsResult
runIntraElimAvailableExpressions(llvm::Function *F, EliminationOptions Opts) {
  return runIntraElimAvailableExpressions(F, nullptr, Opts);
}

AvailableExpressionsResult
runIntraElimAvailableExpressions(llvm::Function *F, llvm::AAResults *AA,
                                 EliminationOptions Opts) {
  return runIntraElimAvailableExpressions(F, AA, nullptr, nullptr, Opts);
}

AvailableExpressionsResult runIntraElimAvailableExpressions(
    llvm::Function *F, llvm::AAResults *AA, llvm::DominatorTree *DT,
    llvm::TargetLibraryInfo *TLI, EliminationOptions Opts) {
  return runIntraElimAvailableExpressions(F, AA, DT, TLI, nullptr, Opts);
}

AvailableExpressionsResult runIntraElimAvailableExpressions(
    llvm::Function *F, llvm::AAResults *AA, llvm::DominatorTree *DT,
    llvm::TargetLibraryInfo *TLI, llvm::MemorySSA *MSSA,
    EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return AvailableExpressionsResult{};
  }

  ElimAvailableExpressionsProblem Problem(F, AA, DT, TLI, MSSA);
  IntraEliminationSolver<LLVMAnalysisTypes<AvailableExpressionsFact, AvailableExpressionsDomain>>
      Solver(Problem, Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  Out.setSolveMetadata(Status, Solver.getDiagnostics());
  return Out;
}

} // namespace elimination

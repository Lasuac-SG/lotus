#include "Dataflow/APA/Analyses/Intra/UninitializedVariables.h"

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

namespace elimination {
namespace {

class ElimUninitVariablesProblem
    : public LLVMIntraEliminationProblem<UninitVariablesFact, UninitializedVariablesDomain> {
public:
  explicit ElimUninitVariablesProblem(llvm::Function *F)
      : LLVMIntraEliminationProblem<UninitVariablesFact, UninitializedVariablesDomain>(F),
        DL(F != nullptr ? &F->getParent()->getDataLayout() : nullptr) {
    buildTransferCache(F);
  }

  ElimUninitVariablesProblem(llvm::Function *F, llvm::AAResults *AA,
                             llvm::AssumptionCache *AC, llvm::DominatorTree *DT)
      : LLVMIntraEliminationProblem<UninitVariablesFact, UninitializedVariablesDomain>(F),
        DL(F != nullptr ? &F->getParent()->getDataLayout() : nullptr), AA(AA),
        AC(AC), DT(DT) {
    buildTransferCache(F);
  }

  UninitVariablesFact
  applyTransfer(const transfer_t &T,
                const UninitVariablesFact &In) const override {
    auto *Inst = T;
    UninitVariablesFact Out = In;
    if (Inst == nullptr) {
      return Out;
    }

    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Inst)) {
      Out.insert(Alloca);
      return Out;
    }

    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
      auto *Ptr = Store->getPointerOperand();
      auto *Val = Store->getValueOperand();
      if (Ptr == nullptr) {
        return Out;
      }
      if (llvm::isa<llvm::UndefValue>(Val) ||
          llvm::isa<llvm::PoisonValue>(Val)) {
        markAliasUninit(Out, Ptr);
        return Out;
      }
      if (GuaranteedInitialized.count(Store)) {
        clearAliasUninit(Out, Ptr);
        clearAliasSetUninit(Out, Ptr);
        return Out;
      }

      auto *StoredInst = llvm::dyn_cast<llvm::Instruction>(Val);
      if (StoredInst != nullptr && In.count(StoredInst)) {
        markAliasUninit(Out, Ptr);
      } else {
        clearAliasUninit(Out, Ptr);
        clearAliasSetUninit(Out, Ptr);
      }
      return Out;
    }

    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
      if (In.count(normalizeMaybePointer(Load->getPointerOperand()))) {
        Out.insert(Load);
      }
      return Out;
    }

    if (auto *Bitcast = llvm::dyn_cast<llvm::BitCastInst>(Inst)) {
      if (In.count(normalizeMaybePointer(Bitcast->getOperand(0)))) {
        Out.insert(Bitcast);
      }
      return Out;
    }

    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Inst)) {
      if (In.count(normalizeMaybePointer(GEP->getPointerOperand()))) {
        Out.insert(GEP);
      }
      return Out;
    }

    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(Inst)) {
      for (auto &IncomingUse : Phi->incoming_values()) {
        auto *Incoming = IncomingUse.get();
        if (In.count(Incoming)) {
          Out.insert(Phi);
          break;
        }
      }
      return Out;
    }

    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(Inst)) {
      if (In.count(normalizeMaybePointer(Select->getTrueValue())) ||
          In.count(normalizeMaybePointer(Select->getFalseValue()))) {
        Out.insert(Select);
      }
      return Out;
    }

    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Inst)) {
      if (In.count(normalizeMaybePointer(Cast->getOperand(0)))) {
        Out.insert(Cast);
      }
      return Out;
    }

    if (auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(Inst)) {
      if (In.count(normalizeMaybePointer(Freeze->getOperand(0)))) {
        Out.insert(Freeze);
      }
      return Out;
    }

    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      handleMemIntrinsics(Call, Out);
      return Out;
    }

    return Out;
  }

  UninitVariablesFact initialFact() const override {
    return this->bottom();
  }

private:
  const llvm::DataLayout *DL;
  llvm::AAResults *AA = nullptr;
  llvm::AssumptionCache *AC = nullptr;
  llvm::DominatorTree *DT = nullptr;
  std::unordered_map<const llvm::Value *, const llvm::Value *> BaseCache;
  std::unordered_map<const llvm::Value *, UninitVariablesFact> BaseClear;
  std::unordered_map<const llvm::Value *, UninitVariablesFact> AliasClear;
  std::unordered_set<const llvm::StoreInst *> GuaranteedInitialized;

  void buildTransferCache(llvm::Function *F) {
    if (F == nullptr || F->isDeclaration())
      return;

    std::vector<llvm::Value *> Values;
    std::unordered_set<llvm::Value *> Seen;
    auto Record = [&](llvm::Value *V) {
      if (V != nullptr && Seen.insert(V).second)
        Values.push_back(V);
    };
    for (auto &Arg : F->args())
      Record(&Arg);
    for (auto &BB : *F) {
      for (auto &I : BB) {
        Record(&I);
        for (auto &Op : I.operands())
          Record(Op.get());
        if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
          if (llvm::isGuaranteedNotToBeUndefOrPoison(
                  Store->getValueOperand(), AC, Store, DT))
            GuaranteedInitialized.insert(Store);
        }
      }
    }

    auto Universe = this->bottom();
    for (auto *V : Values) {
      if (V == nullptr)
        continue;
      Universe.insert(V);
      if (!V->getType()->isPointerTy())
        continue;
      const auto *Base = llvm::getUnderlyingObject(V);
      BaseCache[V] = Base;
      if (Base != nullptr)
        Universe.insert(const_cast<llvm::Value *>(Base));
    }

    for (auto *Candidate : Values) {
      if (Candidate == nullptr || !Candidate->getType()->isPointerTy())
        continue;
      const auto *Base = getBaseObject(Candidate);
      const auto *Norm = Base != nullptr ? Base : Candidate;
      auto It = BaseClear.find(Norm);
      if (It == BaseClear.end())
        It = BaseClear.emplace(Norm, this->bottom()).first;
      It->second.insert(Candidate);
      It->second.insert(const_cast<llvm::Value *>(Norm));
    }

    if (AA == nullptr)
      return;
    for (auto *Ptr : Values) {
      if (Ptr == nullptr || !Ptr->getType()->isPointerTy())
        continue;
      auto Kill = this->bottom();
      llvm::MemoryLocation StoreLoc(
          Ptr, llvm::LocationSize::beforeOrAfterPointer(), llvm::AAMDNodes());
      for (auto *Candidate : Values) {
        if (Candidate == nullptr || !Candidate->getType()->isPointerTy())
          continue;
        llvm::MemoryLocation CandLoc(
            Candidate, llvm::LocationSize::beforeOrAfterPointer(),
            llvm::AAMDNodes());
        if (AA->alias(StoreLoc, CandLoc) != llvm::AliasResult::NoAlias)
          Kill.insert(Candidate);
      }
      AliasClear.emplace(Ptr, std::move(Kill));
    }
  }

  const llvm::Value *getBaseObject(const llvm::Value *V) const {
    (void)DL;
    auto It = BaseCache.find(V);
    if (It != BaseCache.end())
      return It->second;
    return llvm::getUnderlyingObject(V);
  }

  llvm::Value *normalizePointer(llvm::Value *Ptr) const {
    auto *Base = getBaseObject(Ptr);
    return Base != nullptr ? const_cast<llvm::Value *>(Base) : Ptr;
  }

  llvm::Value *normalizeMaybePointer(llvm::Value *V) const {
    if (V != nullptr && V->getType()->isPointerTy()) {
      return normalizePointer(V);
    }
    return V;
  }

  void clearAliasUninit(UninitVariablesFact &Out,
                        const llvm::Value *Ptr) const {
    auto *Base = getBaseObject(Ptr);
    auto *Norm = Base != nullptr ? Base : Ptr;
    auto It = BaseClear.find(Norm);
    if (It != BaseClear.end())
      Out.subtract(It->second);
    else
      Out.erase(const_cast<llvm::Value *>(Norm));
  }

  void markAliasUninit(UninitVariablesFact &Out, llvm::Value *Ptr) const {
    auto *Norm = normalizePointer(Ptr);
    Out.insert(Norm);
  }

  void clearAliasSetUninit(UninitVariablesFact &Out, llvm::Value *Ptr) const {
    if (AA == nullptr || Ptr == nullptr) {
      return;
    }
    auto It = AliasClear.find(Ptr);
    if (It != AliasClear.end())
      Out.subtract(It->second);
  }

  static bool isMemIntrinsic(llvm::Function *Callee, llvm::Intrinsic::ID ID) {
    return Callee != nullptr && Callee->getIntrinsicID() == ID;
  }

  void handleMemIntrinsics(llvm::CallBase *Call,
                           UninitVariablesFact &Out) const {
    auto *Callee = Call->getCalledFunction();
    if (isMemIntrinsic(Callee, llvm::Intrinsic::memset)) {
      if (Call->arg_size() >= 2) {
        auto *Dest = Call->getArgOperand(0);
        auto *Val = Call->getArgOperand(1);
        if (!llvm::isa<llvm::UndefValue>(Val)) {
          clearAliasUninit(Out, Dest);
        } else {
          markAliasUninit(Out, Dest);
        }
      }
      return;
    }
    if (isMemIntrinsic(Callee, llvm::Intrinsic::memcpy) ||
        isMemIntrinsic(Callee, llvm::Intrinsic::memmove)) {
      if (Call->arg_size() >= 2) {
        auto *Dest = Call->getArgOperand(0);
        auto *Src = Call->getArgOperand(1);
        if (Out.count(Src)) {
          markAliasUninit(Out, Dest);
        } else {
          clearAliasUninit(Out, Dest);
        }
      }
      return;
    }
  }
};

} // namespace

UninitVariablesResult runIntraElimUninitVariables(llvm::Function *F,
                                                  EliminationOptions Opts) {
  return runIntraElimUninitVariables(F, nullptr, Opts);
}

UninitVariablesResult runIntraElimUninitVariables(llvm::Function *F,
                                                  llvm::AAResults *AA,
                                                  EliminationOptions Opts) {
  return runIntraElimUninitVariables(F, AA, nullptr, nullptr, Opts);
}

UninitVariablesResult runIntraElimUninitVariables(llvm::Function *F,
                                                  llvm::AAResults *AA,
                                                  llvm::AssumptionCache *AC,
                                                  llvm::DominatorTree *DT,
                                                  EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return UninitVariablesResult{};
  }

  ElimUninitVariablesProblem Problem(F, AA, AC, DT);
  IntraEliminationSolver<LLVMAnalysisTypes<UninitVariablesFact, UninitializedVariablesDomain>> Solver(
      Problem, Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  Out.setSolveMetadata(Status, Solver.getDiagnostics());
  return Out;
}

} // namespace elimination

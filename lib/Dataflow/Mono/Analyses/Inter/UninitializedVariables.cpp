#include "Dataflow/Mono/Analyses/Inter/UninitializedVariables.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/InterSolver.h"

namespace mono {
namespace {

bool isUninitialized(const llvm::Value *Value,
                     const UninitializedVariablesDomain::value_type &Fact) {
  if (Value == nullptr)
    return false;
  if (llvm::isa<llvm::UndefValue>(Value) || llvm::isa<llvm::PoisonValue>(Value))
    return true;
  if (Fact.count(const_cast<llvm::Value *>(Value)))
    return true;
  if (!Value->getType()->isPointerTy())
    return false;
  auto *Base = llvm::getUnderlyingObject(Value);
  for (auto *Candidate : Fact) {
    if (Candidate != nullptr && Candidate->getType()->isPointerTy() &&
        llvm::getUnderlyingObject(Candidate) == Base) {
      return true;
    }
  }
  return false;
}

void clearMemory(llvm::Value *Pointer,
                 UninitializedVariablesDomain::value_type &Fact) {
  auto *Base = llvm::getUnderlyingObject(Pointer);
  std::vector<llvm::Value *> Erase;
  for (auto *Candidate : Fact) {
    if (Candidate == Pointer ||
        (Candidate != nullptr && Candidate->getType()->isPointerTy() &&
         llvm::getUnderlyingObject(Candidate) == Base)) {
      Erase.push_back(Candidate);
    }
  }
  for (auto *Candidate : Erase)
    Fact.erase(Candidate);
}

class InterUninitializedVariablesProblem final
    : public InterMonoProblem<UninitializedVariablesAnalysisTypes> {
public:
  explicit InterUninitializedVariablesProblem(llvm::Function *Entry)
      : InterMonoProblem<UninitializedVariablesAnalysisTypes>({Entry}) {}

  mono_container_t normalFlow(llvm::Instruction *Inst,
                              const mono_container_t &In) override {
    auto Out = In;
    if (auto *Alloca = llvm::dyn_cast_or_null<llvm::AllocaInst>(Inst)) {
      Out.insert(Alloca);
    } else if (auto *Store = llvm::dyn_cast_or_null<llvm::StoreInst>(Inst)) {
      auto *Pointer = Store->getPointerOperand();
      if (isUninitialized(Store->getValueOperand(), In))
        Out.insert(Pointer);
      else
        clearMemory(Pointer, Out);
    } else if (auto *Load = llvm::dyn_cast_or_null<llvm::LoadInst>(Inst)) {
      if (isUninitialized(Load->getPointerOperand(), In))
        Out.insert(Load);
    } else if (auto *Cast = llvm::dyn_cast_or_null<llvm::BitCastInst>(Inst)) {
      if (isUninitialized(Cast->getOperand(0), In))
        Out.insert(Cast);
    } else if (auto *GEP =
                   llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(Inst)) {
      if (isUninitialized(GEP->getPointerOperand(), In))
        Out.insert(GEP);
    } else if (auto *Phi = llvm::dyn_cast_or_null<llvm::PHINode>(Inst)) {
      for (auto &Incoming : Phi->incoming_values()) {
        if (isUninitialized(Incoming.get(), In)) {
          Out.insert(Phi);
          break;
        }
      }
    } else if (auto *Select = llvm::dyn_cast_or_null<llvm::SelectInst>(Inst)) {
      if (isUninitialized(Select->getTrueValue(), In) ||
          isUninitialized(Select->getFalseValue(), In)) {
        Out.insert(Select);
      }
    }
    return Out;
  }

  mono_container_t callFlow(llvm::Instruction *CallSite, llvm::Function *Callee,
                            const mono_container_t &In) override {
    auto Out = In;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr || Callee == nullptr)
      return Out;
    unsigned Index = 0;
    for (auto &Formal : Callee->args()) {
      if (Index >= Call->arg_size())
        break;
      if (isUninitialized(Call->getArgOperand(Index++), In))
        Out.insert(&Formal);
    }
    return Out;
  }

  mono_container_t returnFlow(llvm::Instruction *CallSite, llvm::Function *,
                              llvm::Instruction *ExitStmt, llvm::Instruction *,
                              const mono_container_t &In) override {
    auto Out = In;
    auto *Ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(ExitStmt);
    if (Ret != nullptr && CallSite != nullptr &&
        !CallSite->getType()->isVoidTy() &&
        isUninitialized(Ret->getReturnValue(), In)) {
      Out.insert(CallSite);
    }
    return Out;
  }

  mono_container_t callToRetFlow(llvm::Instruction *, llvm::Instruction *,
                                 llvm::ArrayRef<llvm::Function *>,
                                 const mono_container_t &In) override {
    return In;
  }

  std::unordered_map<llvm::Instruction *, mono_container_t>
  initialSeeds() override {
    std::unordered_map<llvm::Instruction *, mono_container_t> Seeds;
    auto *Entry = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (Entry != nullptr && !Entry->empty())
      Seeds[&Entry->getEntryBlock().front()] = {};
    return Seeds;
  }
};

} // namespace

std::unique_ptr<InterMonoUninitializedVariablesResult>
runInterMonoUninitializedVariables(llvm::Function *Entry) {
  if (Entry == nullptr || Entry->isDeclaration())
    return nullptr;
  InterUninitializedVariablesProblem Problem(Entry);
  InterMonoSolver<UninitializedVariablesAnalysisTypes,
                  kDefaultUninitializedVariablesCallStringLength>
      Solver(Problem);
  Solver.solve();
  const auto *Result = Solver.getResults();
  return Result != nullptr
             ? std::make_unique<InterMonoUninitializedVariablesResult>(*Result)
             : nullptr;
}

} // namespace mono

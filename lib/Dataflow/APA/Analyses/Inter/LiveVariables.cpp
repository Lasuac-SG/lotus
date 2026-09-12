#include "Dataflow/APA/Analyses/Inter/LiveVariables.h"

#include "Dataflow/APA/Analyses/Inter/FlowHelpers.h"
#include "Dataflow/APA/LLVM/InterProblem.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include <unordered_map>

namespace elimination {
namespace {

struct InterLiveVariablesAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = LiveVariablesFact;
  using transfer_t = llvm::Instruction *;
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
  using abstract_domain_t = LiveVariablesDomain;
};

class InterElimLiveVariablesProblem
    : public LLVMInterEliminationProblem<InterLiveVariablesAnalysisTypes> {
public:
  explicit InterElimLiveVariablesProblem(
      llvm::Function *Entry, const dataflow::controlflow::InterCFG *ICF)
      : LLVMInterEliminationProblem<InterLiveVariablesAnalysisTypes>(
            std::vector<llvm::Function *>{Entry}, ICF) {
    auto *M = Entry != nullptr ? Entry->getParent() : nullptr;
    if (M == nullptr)
      return;
    for (auto &F : *M) {
      if (F.isDeclaration())
        continue;
      for (auto &BB : F) {
        for (auto &I : BB) {
          auto &Info = Transfers[&I];
          Info.Gen = this->bottom();
          Info.Kill = this->bottom();
          if (llvm::isa<llvm::DbgInfoIntrinsic>(&I))
            continue;
          if (!I.getType()->isVoidTy())
            Info.Kill.insert(&I);
          for (auto &Op : I.operands()) {
            auto *V = Op.get();
            if (llvm::isa<llvm::Instruction>(V) || llvm::isa<llvm::Argument>(V))
              Info.Gen.insert(V);
          }
        }
      }
    }
  }

  fact_t normalFlow(n_t Inst, const fact_t &In) override {
    fact_t Out = In;
    auto It = Transfers.find(Inst);
    if (It == Transfers.end()) {
      return Out;
    }
    Out.subtract(It->second.Kill);
    Out.unionWith(It->second.Gen);
    return Out;
  }

  ::dataflow::controlflow::FlowDirection direction() const override {
    return ::dataflow::controlflow::FlowDirection::Backward;
  }

  fact_t callFlow(n_t CallSite, f_t Callee, const fact_t &In) override {
    fact_t Out = this->bottom();
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr || Callee == nullptr) {
      return Out;
    }

    llvm_inter::forEachActualFormalPair(Call, Callee,
                                        [&](llvm::Value *Actual,
                                            llvm::Argument *Formal,
                                            unsigned /*Index*/) {
                                          if (In.count(Formal)) {
                                            Out.insert(Actual);
                                          }
                                        });

    llvm_inter::copyGlobalValueFacts(In, Out);
    return Out;
  }

  fact_t returnFlow(n_t CallSite, f_t /*Callee*/, n_t ExitStmt, n_t /*RetSite*/,
                    const fact_t &In) override {
    fact_t Out = this->bottom();
    llvm_inter::copyGlobalValueFacts(In, Out);

    auto *Ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(ExitStmt);
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Ret == nullptr || Call == nullptr) {
      return Out;
    }
    if (auto *RetVal = Ret->getReturnValue()) {
      if (In.count(CallSite) && (llvm::isa<llvm::Instruction>(RetVal) ||
                                 llvm::isa<llvm::Argument>(RetVal))) {
        Out.insert(RetVal);
      }
    }
    return Out;
  }

  fact_t callToRetFlow(n_t CallSite, n_t /*RetSite*/,
                       const std::vector<f_t> & /*Callees*/,
                       const fact_t &In) override {
    return normalFlow(CallSite, In);
  }

  std::unordered_map<n_t, fact_t> initialSeeds() override {
    std::unordered_map<n_t, fact_t> Seeds;
    auto *Entry = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (Entry == nullptr) {
      return Seeds;
    }
    for (auto &BB : *Entry) {
      if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
        Seeds[Ret] = this->bottom();
      }
    }
    return Seeds;
  }

private:
  struct TransferInfo {
    fact_t Gen;
    fact_t Kill;
  };
  std::unordered_map<n_t, TransferInfo> Transfers;
};

} // namespace

InterLiveVariablesResult runInterElimLiveVariables(
    llvm::Function *Entry, const dataflow::controlflow::InterCFG *ICF) {
  InterLiveVariablesResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimLiveVariablesProblem Problem(Entry, ICF);
  InterEliminationSolver<InterLiveVariablesAnalysisTypes,
                         kDefaultInterElimLiveVariablesCallStringLength>
      Solver(Problem);
  auto Status = Solver.solve();
  if (const auto *Res = Solver.getResults()) {
    Out = *Res;
  }
  Out.setSolveStatus(Status);
  return Out;
}

} // namespace elimination

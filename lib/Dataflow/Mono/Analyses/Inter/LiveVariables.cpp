#include "Dataflow/Mono/Analyses/Inter/LiveVariables.h"

#include "llvm/IR/Argument.h"
#include "llvm/IR/Instructions.h"

#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/InterSolver.h"

namespace mono {
namespace {

void mapActualsAndFormals(const llvm::CallBase *Call, llvm::Function *Callee,
                          const LiveVariablesDomain::value_type &In,
                          LiveVariablesDomain::value_type &Out) {
  if (Call == nullptr || Callee == nullptr)
    return;
  unsigned Index = 0;
  for (auto &Formal : Callee->args()) {
    if (Index >= Call->arg_size())
      break;
    auto *Actual = Call->getArgOperand(Index++);
    if (In.count(&Formal))
      Out.insert(Actual);
    if (In.count(Actual))
      Out.insert(&Formal);
  }
}

class InterLiveVariablesProblem final
    : public InterMonoProblem<LiveVariablesAnalysisTypes> {
public:
  explicit InterLiveVariablesProblem(llvm::Function *Entry)
      : InterMonoProblem<LiveVariablesAnalysisTypes>({Entry}) {}

  dataflow::controlflow::FlowDirection direction() const override {
    return dataflow::controlflow::FlowDirection::Backward;
  }

  mono_container_t normalFlow(llvm::Instruction *Inst,
                              const mono_container_t &In) override {
    auto Out = In;
    if (Inst == nullptr)
      return Out;
    if (!Inst->getType()->isVoidTy())
      Out.erase(Inst);
    for (auto &Operand : Inst->operands()) {
      if (llvm::isa<llvm::Instruction>(Operand) ||
          llvm::isa<llvm::Argument>(Operand)) {
        Out.insert(Operand.get());
      }
    }
    return Out;
  }

  mono_container_t callFlow(llvm::Instruction *CallSite, llvm::Function *Callee,
                            const mono_container_t &In) override {
    auto Out = In;
    mapActualsAndFormals(llvm::dyn_cast_or_null<llvm::CallBase>(CallSite),
                         Callee, In, Out);
    return Out;
  }

  mono_container_t returnFlow(llvm::Instruction *CallSite,
                              llvm::Function *Callee, llvm::Instruction *,
                              llvm::Instruction *,
                              const mono_container_t &In) override {
    auto Out = In;
    mapActualsAndFormals(llvm::dyn_cast_or_null<llvm::CallBase>(CallSite),
                         Callee, In, Out);
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
    if (Entry == nullptr)
      return Seeds;
    for (auto &BB : *Entry) {
      if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator()))
        Seeds[Ret] = {};
    }
    return Seeds;
  }
};

} // namespace

std::unique_ptr<InterMonoLiveVariablesResult>
runInterMonoLiveVariables(llvm::Function *Entry) {
  if (Entry == nullptr || Entry->isDeclaration())
    return nullptr;
  InterLiveVariablesProblem Problem(Entry);
  InterMonoSolver<LiveVariablesAnalysisTypes,
                  kDefaultLiveVariablesCallStringLength>
      Solver(Problem);
  Solver.solve();
  const auto *Result = Solver.getResults();
  return Result != nullptr
             ? std::make_unique<InterMonoLiveVariablesResult>(*Result)
             : nullptr;
}

} // namespace mono

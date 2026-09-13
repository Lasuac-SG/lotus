#include "Dataflow/Mono/Analyses/Intra/Taint.h"

#include "llvm/IR/Instructions.h"

#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

namespace mono {
namespace {

class IntraTaintProblem final : public IntraMonoProblem<TaintAnalysisTypes> {
public:
  IntraTaintProblem(llvm::Function *F, const MonoTaintConfig &Config)
      : IntraMonoProblem<TaintAnalysisTypes>({F}), Config(Config) {}

  mono_container_t normalFlow(llvm::Instruction *Inst,
                              const mono_container_t &In) override {
    auto Out = In;
    if (Inst == nullptr)
      return Out;

    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
      if (isTainted(Store->getValueOperand(), In))
        Out.insert(Store->getPointerOperand());
      else
        Out.erase(Store->getPointerOperand());
      return Out;
    }
    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
      if (isTainted(Load->getPointerOperand(), In))
        Out.insert(Load);
      return Out;
    }
    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      auto *Callee = Call->getCalledFunction();
      if (isConfigured(Callee, Config.SourceFunctions)) {
        if (!Call->getType()->isVoidTy())
          Out.insert(Call);
        if (Config.TaintPointerArgsFromSources) {
          for (auto &Argument : Call->args()) {
            if (Argument->getType()->isPointerTy())
              Out.insert(Argument.get());
          }
        }
      }
      if (isConfigured(Callee, Config.SanitizerFunctions))
        Out.erase(Call);
    }

    if (!Inst->getType()->isVoidTy()) {
      for (auto &Operand : Inst->operands()) {
        if (isTainted(Operand.get(), In)) {
          Out.insert(Inst);
          break;
        }
      }
    }
    return Out;
  }

  std::unordered_map<llvm::Instruction *, mono_container_t>
  initialSeeds() override {
    std::unordered_map<llvm::Instruction *, mono_container_t> Seeds;
    auto *F = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (F == nullptr || F->empty())
      return Seeds;
    auto &Initial = Seeds[&F->getEntryBlock().front()];
    if (Config.SeedEntryArguments) {
      for (auto &Argument : F->args())
        Initial.insert(&Argument);
    }
    return Seeds;
  }

private:
  const MonoTaintConfig &Config;

  static bool
  isConfigured(const llvm::Function *F,
               const std::unordered_set<std::string> &ConfiguredFunctions) {
    return F != nullptr && ConfiguredFunctions.count(F->getName().str()) != 0;
  }

  static bool isTainted(const llvm::Value *Value,
                        const mono_container_t &Fact) {
    return Value != nullptr &&
           Fact.count(const_cast<llvm::Value *>(Value)) != 0;
  }
};

} // namespace

std::unique_ptr<DataFlowResult>
runIntraMonoTaint(llvm::Function *F, const MonoTaintConfig &Config) {
  if (F == nullptr || F->isDeclaration())
    return nullptr;
  IntraTaintProblem Problem(F, Config);
  IntraMonoSolver<TaintAnalysisTypes> Solver(Problem);
  Solver.solve();

  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *F) {
    for (auto &I : BB) {
      Result->IN(&I) = Solver.getInResultsAt(&I).getSet();
      Result->OUT(&I) = Solver.getOutResultsAt(&I).getSet();
    }
  }
  return Result;
}

} // namespace mono

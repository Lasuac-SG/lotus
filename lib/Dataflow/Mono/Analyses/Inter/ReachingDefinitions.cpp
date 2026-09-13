#include "Dataflow/Mono/Analyses/Inter/ReachingDefinitions.h"

#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/InterSolver.h"

namespace mono {
namespace {

class InterReachingDefinitionsProblem final
    : public InterMonoProblem<ReachingDefinitionsAnalysisTypes> {
public:
  explicit InterReachingDefinitionsProblem(llvm::Function *Entry)
      : InterMonoProblem<ReachingDefinitionsAnalysisTypes>({Entry}) {}

  mono_container_t normalFlow(llvm::Instruction *Inst,
                              const mono_container_t &In) override {
    auto Out = In;
    if (Inst != nullptr && !Inst->getType()->isVoidTy())
      Out.insert(Inst);
    return Out;
  }

  mono_container_t callFlow(llvm::Instruction *, llvm::Function *,
                            const mono_container_t &In) override {
    return In;
  }

  mono_container_t returnFlow(llvm::Instruction *, llvm::Function *,
                              llvm::Instruction *, llvm::Instruction *,
                              const mono_container_t &In) override {
    return In;
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

std::unique_ptr<InterMonoReachingDefinitionsResult>
runInterMonoReachingDefinitions(llvm::Function *Entry) {
  if (Entry == nullptr || Entry->isDeclaration())
    return nullptr;
  InterReachingDefinitionsProblem Problem(Entry);
  InterMonoSolver<ReachingDefinitionsAnalysisTypes,
                  kDefaultReachingDefinitionsCallStringLength>
      Solver(Problem);
  Solver.solve();
  const auto *Result = Solver.getResults();
  return Result != nullptr
             ? std::make_unique<InterMonoReachingDefinitionsResult>(*Result)
             : nullptr;
}

} // namespace mono

#include "Dataflow/Mono/Analyses/Inter/Reachability.h"

#include "llvm/IR/Instructions.h"

#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/InterSolver.h"

namespace mono {
namespace {

class InterReachabilityProblem final
    : public InterMonoProblem<ReachabilityAnalysisTypes> {
public:
  explicit InterReachabilityProblem(llvm::Function *Entry)
      : InterMonoProblem<ReachabilityAnalysisTypes>({Entry}) {}

  dataflow::controlflow::FlowDirection direction() const override {
    return dataflow::controlflow::FlowDirection::Backward;
  }

  mono_container_t normalFlow(llvm::Instruction *Inst,
                              const mono_container_t &In) override {
    auto Out = In;
    if (Inst != nullptr)
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

std::unique_ptr<InterMonoReachabilityResult>
runInterMonoReachability(llvm::Function *Entry) {
  if (Entry == nullptr || Entry->isDeclaration())
    return nullptr;
  InterReachabilityProblem Problem(Entry);
  InterMonoSolver<ReachabilityAnalysisTypes,
                  kDefaultReachabilityCallStringLength>
      Solver(Problem);
  Solver.solve();
  const auto *Result = Solver.getResults();
  return Result != nullptr
             ? std::make_unique<InterMonoReachabilityResult>(*Result)
             : nullptr;
}

} // namespace mono

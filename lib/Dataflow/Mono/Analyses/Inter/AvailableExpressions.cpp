#include "Dataflow/Mono/Analyses/Inter/AvailableExpressions.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/InterSolver.h"

namespace mono {
namespace {

std::vector<AvailableExpression>
getComputedExpressions(llvm::Instruction *Inst) {
  std::vector<AvailableExpression> Expressions;
  if (auto *Op = llvm::dyn_cast_or_null<llvm::BinaryOperator>(Inst)) {
    llvm::SmallVector<llvm::Value *, 2> Operands{Op->getOperand(0),
                                                 Op->getOperand(1)};
    Expressions.emplace_back(Op->getOpcode(), Operands);
  } else if (auto *Cast = llvm::dyn_cast_or_null<llvm::CastInst>(Inst)) {
    llvm::SmallVector<llvm::Value *, 1> Operands{Cast->getOperand(0)};
    Expressions.emplace_back(Cast->getOpcode(), Operands);
  } else if (auto *Cmp = llvm::dyn_cast_or_null<llvm::CmpInst>(Inst)) {
    llvm::SmallVector<llvm::Value *, 2> Operands{Cmp->getOperand(0),
                                                 Cmp->getOperand(1)};
    Expressions.emplace_back(Cmp->getOpcode(), Operands);
  }
  return Expressions;
}

AvailableExpressionsDomain makeDomain(llvm::Function *Entry) {
  std::set<AvailableExpression> Universe;
  auto *M = Entry != nullptr ? Entry->getParent() : nullptr;
  if (M != nullptr) {
    for (auto &F : *M) {
      for (auto &BB : F) {
        for (auto &I : BB) {
          auto Expressions = getComputedExpressions(&I);
          Universe.insert(Expressions.begin(), Expressions.end());
        }
      }
    }
  }
  return AvailableExpressionsDomain(Universe);
}

class InterAvailableExpressionsProblem final
    : public InterMonoProblem<AvailableExpressionsAnalysisTypes> {
public:
  explicit InterAvailableExpressionsProblem(llvm::Function *Entry)
      : InterMonoProblem<AvailableExpressionsAnalysisTypes>({Entry}, nullptr,
                                                            makeDomain(Entry)) {
  }

  mono_container_t normalFlow(llvm::Instruction *Inst,
                              const mono_container_t &In) override {
    auto Out = In;
    if (Inst == nullptr)
      return Out;
    if (!Inst->getType()->isVoidTy()) {
      for (auto It = Out.begin(); It != Out.end();) {
        if (It->usesValue(Inst))
          It = Out.erase(It);
        else
          ++It;
      }
    }
    auto Generated = getComputedExpressions(Inst);
    Out.insert(Generated.begin(), Generated.end());
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

std::unique_ptr<InterMonoAvailableExpressionsResult>
runInterMonoAvailableExpressions(llvm::Function *Entry) {
  if (Entry == nullptr || Entry->isDeclaration())
    return nullptr;
  InterAvailableExpressionsProblem Problem(Entry);
  InterMonoSolver<AvailableExpressionsAnalysisTypes,
                  kDefaultAvailableExpressionsCallStringLength>
      Solver(Problem);
  Solver.solve();
  const auto *Result = Solver.getResults();
  return Result != nullptr
             ? std::make_unique<InterMonoAvailableExpressionsResult>(*Result)
             : nullptr;
}

} // namespace mono

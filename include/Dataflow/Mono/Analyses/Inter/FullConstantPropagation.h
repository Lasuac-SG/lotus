#pragma once

#include "Dataflow/Mono/Domains/FullConstantPropagationDomain.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

constexpr unsigned kDefaultFullConstantPropagationCallStringLength = 2;
using InterMonoFullConstantPropagationResult =
    dataflow::ContextSensitiveDataFlowResult<
        kDefaultFullConstantPropagationCallStringLength,
        FullConstantPropagationState>;

struct InterMonoFullConstantPropagationAnalysisResult {
  std::unique_ptr<InterMonoFullConstantPropagationResult> Results;
};

InterMonoFullConstantPropagationAnalysisResult
runInterMonoFullConstantPropagation(llvm::Function *Entry);

} // namespace mono

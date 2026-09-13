#pragma once

#include "Dataflow/Mono/Domains/ConstantPropagationDomain.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

constexpr unsigned kDefaultConstantPropagationCallStringLength = 2;
using InterMonoConstantPropagationResult =
    dataflow::ContextSensitiveDataFlowResult<
        kDefaultConstantPropagationCallStringLength, ConstantPropagationMap>;

struct InterMonoConstantPropagationAnalysisResult {
  std::unique_ptr<InterMonoConstantPropagationResult> Results;
};

// Interprocedural constant propagation (call-string length is fixed at 2).
InterMonoConstantPropagationAnalysisResult
runInterMonoConstantPropagation(llvm::Function *Entry);

} // namespace mono

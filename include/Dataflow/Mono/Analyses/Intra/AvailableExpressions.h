#pragma once

#include "Dataflow/Mono/Domains/AvailableExpressionsDomain.h"
#include "Dataflow/Mono/Support/Result.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

/// Run the forward, must-style available-expressions analysis on a function.
std::unique_ptr<DataFlowResult>
runIntraMonoAvailableExpressions(llvm::Function *F);

} // namespace mono

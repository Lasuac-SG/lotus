#pragma once

#include "Dataflow/Mono/Domains/AvailableExpressionsDomain.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

constexpr unsigned kDefaultAvailableExpressionsCallStringLength = 2;
using InterMonoAvailableExpressionsResult =
    dataflow::ContextSensitiveDataFlowResult<
        kDefaultAvailableExpressionsCallStringLength,
        AvailableExpressionsDomain::value_type>;

std::unique_ptr<InterMonoAvailableExpressionsResult>
runInterMonoAvailableExpressions(llvm::Function *Entry);

} // namespace mono

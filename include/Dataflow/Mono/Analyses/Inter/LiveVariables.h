#pragma once

#include "Dataflow/Mono/Domains/LiveVariablesDomain.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

constexpr unsigned kDefaultLiveVariablesCallStringLength = 2;
using InterMonoLiveVariablesResult = dataflow::ContextSensitiveDataFlowResult<
    kDefaultLiveVariablesCallStringLength, LiveVariablesDomain::value_type>;

std::unique_ptr<InterMonoLiveVariablesResult>
runInterMonoLiveVariables(llvm::Function *Entry);

} // namespace mono

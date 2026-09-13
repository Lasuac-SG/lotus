#pragma once

#include "Dataflow/Mono/Domains/UninitializedVariablesDomain.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

constexpr unsigned kDefaultUninitializedVariablesCallStringLength = 2;
using InterMonoUninitializedVariablesResult =
    dataflow::ContextSensitiveDataFlowResult<
        kDefaultUninitializedVariablesCallStringLength,
        UninitializedVariablesDomain::value_type>;

std::unique_ptr<InterMonoUninitializedVariablesResult>
runInterMonoUninitializedVariables(llvm::Function *Entry);

} // namespace mono

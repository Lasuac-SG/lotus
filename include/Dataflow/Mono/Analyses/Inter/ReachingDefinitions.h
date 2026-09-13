#pragma once

#include "Dataflow/Mono/Domains/ReachingDefinitionsDomain.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

constexpr unsigned kDefaultReachingDefinitionsCallStringLength = 2;
using InterMonoReachingDefinitionsResult =
    dataflow::ContextSensitiveDataFlowResult<
        kDefaultReachingDefinitionsCallStringLength,
        ReachingDefinitionsDomain::value_type>;

std::unique_ptr<InterMonoReachingDefinitionsResult>
runInterMonoReachingDefinitions(llvm::Function *Entry);

} // namespace mono

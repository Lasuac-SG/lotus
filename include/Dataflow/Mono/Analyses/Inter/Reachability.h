#pragma once

#include "Dataflow/Mono/Domains/ReachabilityDomain.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

constexpr unsigned kDefaultReachabilityCallStringLength = 2;
using InterMonoReachabilityResult = dataflow::ContextSensitiveDataFlowResult<
    kDefaultReachabilityCallStringLength, ReachabilityDomain::value_type>;

std::unique_ptr<InterMonoReachabilityResult>
runInterMonoReachability(llvm::Function *Entry);

} // namespace mono

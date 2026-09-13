#pragma once

#include "Dataflow/Mono/Domains/ReachabilityDomain.h"
#include "Dataflow/Mono/Support/MonoDebug.h"
#include "Dataflow/Mono/Support/Result.h"

#include <functional>
#include <memory>

namespace llvm {
class Function;
class Instruction;
} // namespace llvm

namespace mono {

// Compute forward reachability using backward dataflow analysis.
// This analysis determines which instructions can be executed from each program
// point.
std::unique_ptr<DataFlowResult>
runIntraMonoReachability(llvm::Function *f,
                         const DebugConfig &DebugCfg = DebugConfig{});

std::unique_ptr<DataFlowResult> runIntraMonoReachability(
    llvm::Function *f, const std::function<bool(llvm::Instruction *i)> &filter,
    const DebugConfig &DebugCfg = DebugConfig{});

} // namespace mono

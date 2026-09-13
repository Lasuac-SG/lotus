#pragma once

#include "Dataflow/Mono/Domains/UninitializedVariablesDomain.h"
#include "Dataflow/Mono/Support/MonoDebug.h"
#include "Dataflow/Mono/Support/Result.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

// Forward uninitialized variables analysis (intraprocedural).
std::unique_ptr<DataFlowResult>
runIntraMonoUninitializedVariables(llvm::Function *F,
                                   const DebugConfig &DebugCfg = DebugConfig{});

} // namespace mono

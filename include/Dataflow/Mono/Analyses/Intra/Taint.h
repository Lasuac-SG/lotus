#pragma once

#include "Dataflow/Mono/Domains/TaintDomain.h"
#include "Dataflow/Mono/Support/Result.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

std::unique_ptr<DataFlowResult>
runIntraMonoTaint(llvm::Function *F, const MonoTaintConfig &Config = {});

} // namespace mono

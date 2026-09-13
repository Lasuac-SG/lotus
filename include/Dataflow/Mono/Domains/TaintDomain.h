#pragma once

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

#include <string>
#include <unordered_set>

namespace llvm {
class Value;
} // namespace llvm

namespace mono {

struct MonoTaintConfig {
  std::unordered_set<std::string> SourceFunctions;
  std::unordered_set<std::string> SinkFunctions;
  std::unordered_set<std::string> SanitizerFunctions;
  bool SeedEntryArguments = false;
  bool TaintPointerArgsFromSources = true;
};

struct TaintDomain : UnionDomain<SetContainer<llvm::Value *>> {};

using TaintAnalysisTypes =
    LLVMMonoAnalysisTypes<TaintDomain::value_type, TaintDomain>;

} // namespace mono

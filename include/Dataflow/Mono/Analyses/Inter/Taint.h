#pragma once

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/Domains/TaintDomain.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"

#include <map>
#include <memory>
#include <set>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace mono {

struct InterMonoTaintReport {
  std::map<llvm::Instruction *, std::set<llvm::Value *>> Leaks;
};

constexpr unsigned kDefaultTaintCallStringLength = 2;
using InterMonoTaintResult =
    dataflow::ContextSensitiveDataFlowResult<kDefaultTaintCallStringLength,
                                             SetContainer<llvm::Value *>>;

struct InterMonoTaintAnalysisResult {
  std::unique_ptr<InterMonoTaintResult> Results;
  InterMonoTaintReport Report;
};

// Interprocedural taint analysis (call-string length is fixed at 2).
InterMonoTaintAnalysisResult runInterMonoTaint(llvm::Function *Entry,
                                               const MonoTaintConfig &Config);

} // namespace mono

#pragma once

#include "Dataflow/NPA/Domains/TaintDomain.h"
#include "Dataflow/NPA/LLVM/AnalysisSupport.h"

#include <map>

#include <llvm/IR/Module.h>

namespace npa {

class InterMaybeUninitialized {
public:
  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, TaintTransformer::value_type> summaries;
    std::map<BlockKey, TaintTransformer::fact_type> blockFacts;
  };

  static Result
  run(llvm::Module &M, bool verbose = false,
      LinearStrategy linearStrategy = LinearStrategy::SCC,
      IndirectCallResolutionMode callResolutionMode =
          IndirectCallResolutionMode::ClosedWorldTypeCompatible,
      NewtonRoundStrategy roundStrategy = NewtonRoundStrategy::Dense);
};

} // namespace npa

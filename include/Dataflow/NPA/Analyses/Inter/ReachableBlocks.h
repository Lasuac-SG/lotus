#pragma once

#include "llvm/IR/Module.h"

#include "Dataflow/NPA/Domains/GenKillDomain.h"
#include "Dataflow/NPA/LLVM/AnalysisSupport.h"

#include <map>
#include <set>

namespace npa {

class InterReachableBlocks {
public:
  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, GenKillTransformer::value_type> summaries;
    std::set<const llvm::BasicBlock *> reachableBlocks;
  };

  static Result
  run(llvm::Module &M, bool verbose = false,
      LinearStrategy linearStrategy = LinearStrategy::SCC,
      IndirectCallResolutionMode callResolutionMode =
          IndirectCallResolutionMode::ClosedWorldTypeCompatible,
      NewtonRoundStrategy roundStrategy = NewtonRoundStrategy::Dense);
};

} // namespace npa

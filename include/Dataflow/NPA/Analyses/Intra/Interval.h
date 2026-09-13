#pragma once

#include "llvm/IR/Function.h"

#include "Dataflow/NPA/Analyses/Inter/Interval.h"

namespace npa {

class IntraIntervalAnalysis {
public:
  using Result = InterIntervalAnalysis::Result;

  static Result
  run(llvm::Function &F, bool verbose = false,
      LinearStrategy linearStrategy = LinearStrategy::SCC,
      NewtonRoundStrategy roundStrategy = NewtonRoundStrategy::Dense);
};

} // namespace npa

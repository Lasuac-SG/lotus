#pragma once

#include "llvm/IR/Function.h"

#include "Dataflow/NPA/Analyses/Inter/ConstantPropagation.h"

namespace npa {

class ConstantPropagation {
public:
  using Result = InterConstantPropagation::Result;

  static Result
  run(llvm::Function &F, bool verbose = false,
      LinearStrategy linearStrategy = LinearStrategy::SCC,
      NewtonRoundStrategy roundStrategy = NewtonRoundStrategy::Dense);
};

} // namespace npa

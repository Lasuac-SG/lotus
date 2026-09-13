#pragma once

#include "llvm/IR/Function.h"

#include "Dataflow/NPA/Analyses/Inter/MaybeUninitialized.h"

namespace npa {

class MaybeUninitialized {
public:
  using Result = InterMaybeUninitialized::Result;

  static Result
  run(llvm::Function &F, bool verbose = false,
      LinearStrategy linearStrategy = LinearStrategy::SCC,
      NewtonRoundStrategy roundStrategy = NewtonRoundStrategy::Dense);
};

} // namespace npa

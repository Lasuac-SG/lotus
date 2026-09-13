#pragma once

#include "llvm/IR/Function.h"

#include "Dataflow/NPA/Analyses/Inter/Nullability.h"

namespace npa {

class Nullability {
public:
  using Options = InterNullability::Options;
  using Result = InterNullability::Result;

  static Result run(llvm::Function &F,
                    lotus::AliasAnalysisWrapper &aliasAnalysis,
                    const Options &options, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC);
  static Result run(llvm::Function &F, const Options &options,
                    bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC);
};

} // namespace npa

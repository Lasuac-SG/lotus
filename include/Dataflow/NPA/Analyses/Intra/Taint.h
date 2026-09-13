#pragma once

#include "llvm/IR/Function.h"

#include "Dataflow/NPA/Analyses/Inter/Taint.h"

namespace npa {

class Taint {
public:
  using Options = InterTaint::Options;
  using Result = InterTaint::Result;

  static Result run(llvm::Function &F,
                    lotus::AliasAnalysisWrapper &aliasAnalysis,
                    const Options &options, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC);
};

} // namespace npa

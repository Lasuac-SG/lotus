#include "Dataflow/NPA/Analyses/Intra/Nullability.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

namespace npa {
namespace detail {
InterNullability::Result runIntraNullability(llvm::Function &,
                                             lotus::AliasAnalysisWrapper &,
                                             const InterNullability::Options &,
                                             bool, LinearStrategy);
} // namespace detail

Nullability::Result Nullability::run(llvm::Function &F,
                                     lotus::AliasAnalysisWrapper &aliasAnalysis,
                                     const Options &options, bool verbose,
                                     LinearStrategy linearStrategy) {
  return detail::runIntraNullability(F, aliasAnalysis, options, verbose,
                                     linearStrategy);
}

Nullability::Result Nullability::run(llvm::Function &F, const Options &options,
                                     bool verbose,
                                     LinearStrategy linearStrategy) {
  lotus::AliasAnalysisWrapper aliasAnalysis(*F.getParent(),
                                            lotus::AAConfig::BasicAA());
  return run(F, aliasAnalysis, options, verbose, linearStrategy);
}

} // namespace npa

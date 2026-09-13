#include "Dataflow/NPA/Analyses/Intra/Taint.h"

namespace npa {
namespace detail {
InterTaint::Result runIntraTaint(llvm::Function &,
                                 lotus::AliasAnalysisWrapper &,
                                 const InterTaint::Options &, bool,
                                 LinearStrategy);
} // namespace detail

Taint::Result Taint::run(llvm::Function &F,
                         lotus::AliasAnalysisWrapper &aliasAnalysis,
                         const Options &options, bool verbose,
                         LinearStrategy linearStrategy) {
  return detail::runIntraTaint(F, aliasAnalysis, options, verbose,
                               linearStrategy);
}

} // namespace npa

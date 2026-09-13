#include "Dataflow/NPA/Analyses/Intra/Interval.h"

namespace npa {
namespace detail {
InterIntervalAnalysis::Result
runIntraInterval(llvm::Function &, bool, LinearStrategy, NewtonRoundStrategy);
} // namespace detail

IntraIntervalAnalysis::Result
IntraIntervalAnalysis::run(llvm::Function &F, bool verbose,
                           LinearStrategy linearStrategy,
                           NewtonRoundStrategy roundStrategy) {
  return detail::runIntraInterval(F, verbose, linearStrategy, roundStrategy);
}

} // namespace npa

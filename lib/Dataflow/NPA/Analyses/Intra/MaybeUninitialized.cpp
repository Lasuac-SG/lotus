#include "Dataflow/NPA/Analyses/Intra/MaybeUninitialized.h"

namespace npa {
namespace detail {
InterMaybeUninitialized::Result runIntraMaybeUninitialized(llvm::Function &,
                                                           bool, LinearStrategy,
                                                           NewtonRoundStrategy);
}

MaybeUninitialized::Result
MaybeUninitialized::run(llvm::Function &F, bool verbose,
                        LinearStrategy linearStrategy,
                        NewtonRoundStrategy roundStrategy) {
  return detail::runIntraMaybeUninitialized(F, verbose, linearStrategy,
                                            roundStrategy);
}

} // namespace npa

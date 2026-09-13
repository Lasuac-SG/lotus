#include "Dataflow/NPA/Analyses/Intra/ConstantPropagation.h"

namespace npa {
namespace detail {
InterConstantPropagation::Result
runIntraConstantPropagation(llvm::Function &, bool, LinearStrategy,
                            NewtonRoundStrategy);
} // namespace detail

ConstantPropagation::Result
ConstantPropagation::run(llvm::Function &F, bool verbose,
                         LinearStrategy linearStrategy,
                         NewtonRoundStrategy roundStrategy) {
  return detail::runIntraConstantPropagation(F, verbose, linearStrategy,
                                             roundStrategy);
}

} // namespace npa

#ifndef NPA_SOLVER_OPTIONS_H
#define NPA_SOLVER_OPTIONS_H

#include "Dataflow/NPA/Core/Domain.h"

namespace npa {

/// Backend for the linearized equation system inside a Newton round.
enum class LinearStrategy {
  Naive,
  SCC,
  AdaptiveScc,
  TensorProduct,
};

enum class DomainContractMode {
  Off,
  BasicChecks,
  Strict,
};

struct SolveOptions {
  bool verbose = false;
  int max_iterations = -1;
  LinearStrategy linear_strategy = LinearStrategy::SCC;
  DomainContractMode contract_mode = DomainContractMode::Off;
  ConvergencePolicy convergence_policy = ConvergencePolicy::DomainDefault;
};

} // namespace npa

#endif // NPA_SOLVER_OPTIONS_H

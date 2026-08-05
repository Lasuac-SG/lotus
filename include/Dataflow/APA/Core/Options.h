#ifndef DATAFLOW_ELIMINATION_CORE_OPTIONS_H_
#define DATAFLOW_ELIMINATION_CORE_OPTIONS_H_

#include <cstddef>

#include "Dataflow/APA/EAN/Budget.h"
#include "Dataflow/APA/EAN/CostModel.h"
#include "Dataflow/APA/EAN/ExtractOptions.h"
#include "Dataflow/APA/EAN/LawProfile.h"

namespace elimination {

enum class EliminationMethod {
  // Generic O(n^3) state-elimination (Floyd–Warshall-style) over all nodes.
  StateElimination,
  // Paper-style ADT "simple" algorithm path-expression updates.
  ADTSimple,
  // Paper-style ADT + path-expression construction (requires reducible info).
  ADTDelayed,
};

// Pivot-order policy for the state-elimination engine (paper's "Order"
// configuration). The order never changes the final all-pairs result (the
// k-loop is a Floyd–Warshall closure), only the peak construction cost — see
// Solver/EliminationOrder.h.
enum class OrderingPolicy {
  // Baseline: reverse-topological order for reducible problems, identity
  // otherwise (unchanged historical behavior).
  Default,
  // Cost-aware greedy minimum-product ordering that minimizes the
  // predecessor–successor product driving Eq. 1's intermediate growth.
  CostAware,
};

enum class OnNonConvergentStar {
  // Abort solve with NonConvergentStar status.
  Fail,
  // Return the last iterand at the iteration bound.
  ReturnLast,
  // Return meet identity at the iteration bound.
  ReturnIdentity,
};

enum class SolveStatus {
  Ok,
  FallbackToState,
  NonConvergentStar,
  InvalidProblem,
};

enum class FallbackReason {
  None,
  ADTRejected,
  InvalidProblem,
};

struct SolveDiagnostics final {
  bool used_adt = false;
  EliminationMethod requested_method = EliminationMethod::StateElimination;
  EliminationMethod executed_method = EliminationMethod::StateElimination;
  FallbackReason fallback_reason = FallbackReason::None;
  std::size_t star_iterations_total = 0;
  bool max_star_hit = false;
  // Peak unique path-expression DAG nodes reachable from the WHOLE elimination
  // matrix at any point during construction (state elimination only). Populated
  // only when EliminationOptions::MeasurePeakNodes is set; 0 otherwise. This is
  // the paper's RQ3 "peak construction nodes" metric.
  std::size_t peak_matrix_nodes = 0;
};

struct EliminationOptions final {
  EliminationMethod Method = EliminationMethod::StateElimination;
  // Pivot-order policy for the state-elimination engine. Default preserves the
  // historical baseline order; CostAware selects the paper's "Order" policy.
  OrderingPolicy Ordering = OrderingPolicy::Default;
  OnNonConvergentStar NonConvergentStarPolicy = OnNonConvergentStar::Fail;
  // 0 means "use Problem.maxStarIterations()".
  std::size_t MaxStarIterations = 0;
  // Reserved for future conditional collection. Diagnostics are currently
  // recorded unconditionally by the solver and attached to result metadata.
  bool RecordDiagnostics = true;

  // Opt-in RQ3 instrumentation: when set, the state-elimination engine records
  // SolveDiagnostics::peak_matrix_nodes (peak unique DAG nodes across the whole
  // matrix during construction). Default off — adds a per-pivot reachability
  // scan, so it is enabled only by the evaluation harness.
  bool MeasurePeakNodes = false;

  // EAN (Equality-saturation Algebraic Normalizer) post-optimization. When
  // enabled, the solver runs EAN on the batch of path-expression summaries
  // before interpreting them (see SolverContext::applyEAN). Default off, so the
  // baseline "Default" configuration is unchanged. The default law profile is
  // universally safe (left distributivity only); distributive clients may set a
  // richer profile (RightDistributive/Sliding/...) per their algebra.
  bool EnableEAN = false;
  ean::LawProfile EANLaws = ean::LawProfile::safeMinimal();
  ean::CostModel EANCost = ean::CostModel::uniform();
  ean::Budget EANBudget = ean::Budget::unbounded();
  ean::ExtractOptions EANExtract = {};
};

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_CORE_OPTIONS_H_

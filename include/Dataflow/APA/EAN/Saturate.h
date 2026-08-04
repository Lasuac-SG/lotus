#ifndef DATAFLOW_APA_EAN_SATURATE_H_
#define DATAFLOW_APA_EAN_SATURATE_H_

// Saturate: the guarded, budgeted, anytime saturation driver (paper
// Algorithm 1). It runs the phases [Cleanup, Factor, Star, Explore]; within a
// phase it repeats {apply-round, rebuild (done by the pass), extract-cost,
// track-plateau} until a phase saturates or a budget bound is hit.
//
// M3 status: only FACTOR is a real rewrite (M2). CLEANUP is already enforced by
// canon:: at construction, and STAR/EXPLORE arrive in M5, so those phases are
// no-op slots for now. The value delivered here is the R2 machinery: bounded
// exploration with a valid result at every stopping point.
//
// Because the e-graph only grows (saturation adds equivalent forms, never
// removes), the best extractable candidate always comes from the final graph —
// so per-round extraction is used only for the plateau signal, and the actual
// best is extracted once, at the end (see EAN.h).

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <vector>

#include "Dataflow/APA/EAN/Budget.h"
#include "Dataflow/APA/EAN/Canonical.h"
#include "Dataflow/APA/EAN/Factorize.h"
#include "Dataflow/APA/EAN/LawProfile.h"
#include "Solvers/EGraph/Analysis.h"
#include "Solvers/EGraph/Extract.h"

namespace elimination {
namespace ean {

enum class Phase { Cleanup, Factor, Star, Explore };

// One round of a phase. Returns the number of new equivalent forms added.
inline std::size_t applyPhaseRound(Phase p, Graph &g, const LawProfile &L) {
  switch (p) {
  case Phase::Factor:
    return factorizeRound(g, L);
  case Phase::Cleanup: // canonical laws enforced at construction (M1)
  case Phase::Star:    // M5
  case Phase::Explore: // M5
    return 0;
  }
  return 0;
}

// Client-weighted tree cost of the current best extraction, summed over roots.
// Used as the plateau signal (cross-root sharing is double-counted, which is
// fine for a monotone stop heuristic; the real objective arrives in M4).
template <typename CostFn>
double extractCost(const Graph &g, const std::vector<Id> &roots,
                   const CostFn &cost_fn) {
  ::lotus::egraph::Extractor<PathLang, ::lotus::egraph::NoAnalysis<PathLang>,
                             CostFn>
      ex(g, cost_fn);
  double total = 0.0;
  for (Id r : roots) {
    total += static_cast<double>(ex.findBestCost(r));
  }
  return total;
}

// Run the phased, budgeted saturation in place on `g`. `costEval(g, roots)`
// returns the current scalar cost (the plateau signal) — inject the tree cost
// or the reuse-aware Eq. 5 cost to switch plateau modes. Returns metrics.
template <typename CostEval>
SaturationStats saturate(Graph &g, const std::vector<Id> &roots,
                         const LawProfile &L, const Budget &B,
                         CostEval &&costEval) {
  SaturationStats st;
  st.peakNodes = g.totalSize();
  st.initCost = costEval(g, roots);
  double best = st.initCost;
  std::size_t plateau = 0;
  const auto start = std::chrono::steady_clock::now();

  bool stop = false;
  for (Phase p : {Phase::Cleanup, Phase::Factor, Phase::Star, Phase::Explore}) {
    while (!stop) {
      const std::size_t changed = applyPhaseRound(p, g, L);
      if (changed == 0) {
        break; // this phase has saturated; advance to the next phase
      }
      ++st.rounds;
      st.peakNodes = std::max(st.peakNodes, g.totalSize());

      const double c = costEval(g, roots);
      if (c < best) {
        best = c;
        plateau = 0;
      } else {
        ++plateau;
      }

      const double elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
              .count();
      if (st.rounds >= B.roundLimit) {
        st.stop = StopReason::RoundLimit;
        stop = true;
      } else if (g.totalSize() >= B.nodeLimit) {
        st.stop = StopReason::NodeLimit;
        stop = true;
      } else if (plateau >= B.plateauLimit) {
        st.stop = StopReason::Plateau;
        stop = true;
      } else if (elapsed >= B.timeLimitSec) {
        st.stop = StopReason::TimeLimit;
        stop = true;
      }
    }
    if (stop) {
      break;
    }
  }

  st.finalCost = best;
  return st;
}

} // namespace ean
} // namespace elimination

#endif // DATAFLOW_APA_EAN_SATURATE_H_

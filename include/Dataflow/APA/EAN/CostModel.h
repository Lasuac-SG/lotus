#ifndef DATAFLOW_APA_EAN_COSTMODEL_H_
#define DATAFLOW_APA_EAN_COSTMODEL_H_

// CostModel: per-operator weights for extraction (paper §III.D). A client that
// prices composition/closure highly (e.g. a transition-formula algebra) sets
// wSeq/wStar large; a bit-vector client uses near-uniform weights.
//
// PathCostFn plugs these weights into the egg Extractor's cost interface. This
// is the "profiled tree cost" used as the M3 extraction and as the RQ4
// baseline; the reuse-aware, non-additive DAG objective (Eq. 5) is added in M4.

#include "Dataflow/APA/EAN/PathLang.h"
#include "Solvers/EGraph/Extract.h"

namespace elimination {
namespace ean {

struct CostModel {
  double wJoin = 1.0; // ⊕   (per-operator weights w_o = the operator term)
  double wSeq = 1.0;  // ·
  double wStar = 1.0; // *
  double wAtom = 1.0;
  double wZero = 0.0;
  double wOne = 0.0;

  // Eq. 5 shared-DAG objective weights (used by the reuse-aware extractor):
  //   C_DAG = alpha*N_unique + beta*E_unique + sum_o w_o*N_o + gamma*C_repeat.
  double alpha = 1.0; // unique DAG nodes (retained memory)
  double beta = 0.0;  // unique DAG edges (factory construction)
  double gamma = 0.0; // repeated work under a non-memoizing interpreter

  // Uniform structural weights (approximates unique-node count).
  static CostModel uniform() { return CostModel{}; }

  // A composition/closure-heavy profile: star > seq > join.
  static CostModel profiled() {
    CostModel m;
    m.wStar = 4.0;
    m.wSeq = 2.0;
    m.wJoin = 1.0;
    m.wAtom = 1.0;
    m.wZero = 0.0;
    m.wOne = 0.0;
    return m;
  }
};

// egg-compatible cost function: weighted node cost summed over children (tree
// cost). The Extractor turns this into a per-e-class best node/cost.
struct PathCostFn
    : ::lotus::egraph::CostFunction<PathCostFn, PathLang, double> {
  using Cost = double;

  CostModel model;

  PathCostFn() = default;
  explicit PathCostFn(CostModel m) : model(m) {}

  double opWeight(const PathLang &n) const {
    if (isZero(n)) return model.wZero;
    if (isOne(n)) return model.wOne;
    if (isAtom(n)) return model.wAtom;
    if (isStar(n)) return model.wStar;
    if (isJoin(n)) return model.wJoin;
    return model.wSeq; // seq
  }

  template <typename C> double cost(const PathLang &node, C &&child_cost) {
    double total = opWeight(node);
    for (Id child : node.children()) {
      total += child_cost(child);
    }
    return total;
  }
};

} // namespace ean
} // namespace elimination

#endif // DATAFLOW_APA_EAN_COSTMODEL_H_

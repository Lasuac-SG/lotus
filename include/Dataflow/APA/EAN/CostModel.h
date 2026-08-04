#ifndef DATAFLOW_APA_EAN_COSTMODEL_H_
#define DATAFLOW_APA_EAN_COSTMODEL_H_

// CostModel: per-operator weights (paper §III.D) plus the Eq. 5 shared-DAG
// objective weights. This is plain data with NO e-graph/egg dependency, so it
// can be embedded in EliminationOptions (Core) without pulling egg headers into
// Core. The egg-facing cost function that uses these weights is PathCostFn,
// defined in CostFn.h.

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

} // namespace ean
} // namespace elimination

#endif // DATAFLOW_APA_EAN_COSTMODEL_H_

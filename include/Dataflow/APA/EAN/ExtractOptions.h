#ifndef DATAFLOW_APA_EAN_EXTRACTOPTIONS_H_
#define DATAFLOW_APA_EAN_EXTRACTOPTIONS_H_

// Knobs for reuse-aware batch extraction (paper Algorithm 2) and for how the
// saturation driver measures its plateau signal.

#include <cstddef>

namespace elimination {
namespace ean {

struct ExtractOptions {
  std::size_t reuseIters = 3;   // K: reuse-refinement iterations
  double discountLambda = 0.5;  // λ: sublinear sharing discount rate
  std::size_t relaxPasses = 0;  // cycle-safe relaxation passes; 0 = auto (#classes+1)

  // Which cost the saturation loop uses to detect a plateau. Tree is cheap
  // (M3 egg tree cost); Dag runs the full reuse-aware Eq. 5 objective each
  // round (faithful to Algorithm 1, but K·I× more expensive). Switchable so the
  // choice can be made from evaluation data (RQ4).
  enum class PlateauCost { Tree, Dag };
  PlateauCost plateauMode = PlateauCost::Tree;
};

} // namespace ean
} // namespace elimination

#endif // DATAFLOW_APA_EAN_EXTRACTOPTIONS_H_

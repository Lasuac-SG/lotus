// EAN step-3 tests: guarded expansion (Explore phase) and the unscheduled
// saturation mode. Expansion must (a) fire only when it aligns with structure
// already in the e-graph, (b) preserve client semantics, and (c) leave results
// unchanged under both the scheduled and unscheduled drivers.

#include "Dataflow/APA/EAN/Canonical.h"
#include "Dataflow/APA/EAN/EAN.h"
#include "Dataflow/APA/EAN/Expand.h"
#include "Dataflow/APA/EAN/Export.h"
#include "Dataflow/APA/EAN/Import.h"

#include "BoolKleene.h"

#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace lotus_test_ean;
namespace ean = elimination::ean;
using ean::CostModel;
using ean::ExtractOptions;
using ean::LawProfile;
using Factory = elimination::PathExprFactory<int>;
using Ref = Factory::Ref;

// Expansion fires when a produced branch (a·c) already exists as a shared class.
TEST(EanExpand, FiresOnAlignedSharing) {
  Factory F;
  Ref a = F.atom(0), c = F.atom(1), d = F.atom(2);
  Ref r1 = F.concat(a, F.unite(c, d)); // a·(c⊕d)
  Ref r2 = F.concat(a, c);             // a·c  — the aligned product
  auto imp = ean::importCanonical<int>({r1, r2});

  ExtractOptions opts; // expandMinAligned = 1
  const std::size_t ch =
      ean::expandRound(imp.g, LawProfile::kleeneAlgebra(), opts);
  EXPECT_GT(ch, 0u) << "expansion should fire when a·c already exists";
}

// Expansion is rejected when no produced branch aligns with existing structure.
TEST(EanExpand, RejectsUnalignedExpansion) {
  Factory F;
  Ref a = F.atom(0), c = F.atom(1), d = F.atom(2);
  Ref r1 = F.concat(a, F.unite(c, d)); // a·(c⊕d); a·c / a·d exist nowhere else
  auto imp = ean::importCanonical<int>({r1});

  ExtractOptions opts; // expandMinAligned = 1
  const std::size_t ch =
      ean::expandRound(imp.g, LawProfile::kleeneAlgebra(), opts);
  EXPECT_EQ(ch, 0u) << "no aligned branch → expansion must be rejected";
}

// Disabling the alignment requirement (min=0) admits the expansion.
TEST(EanExpand, MinAlignedZeroAdmits) {
  Factory F;
  Ref a = F.atom(0), c = F.atom(1), d = F.atom(2);
  Ref r1 = F.concat(a, F.unite(c, d));
  auto imp = ean::importCanonical<int>({r1});

  ExtractOptions opts;
  opts.expandMinAligned = 0;
  const std::size_t ch =
      ean::expandRound(imp.g, LawProfile::kleeneAlgebra(), opts);
  EXPECT_GT(ch, 0u) << "min-aligned 0 removes the alignment guard";
}

// End-to-end ean() preserves client facts with the Explore phase active, in both
// the scheduled and unscheduled drivers.
TEST(EanExpand, PreservesSemanticsScheduledAndUnscheduled) {
  Factory F;
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2), d = F.atom(3);
  Ref r1 = F.concat(a, F.unite(c, d));                    // a·(c⊕d)
  Ref r2 = F.concat(a, c);                                // a·c
  Ref r3 = F.unite(F.concat(a, c), F.concat(b, c));       // a·c ⊕ b·c
  std::vector<Ref> R = {r1, r2, r3};
  std::vector<Mat> before;
  for (const auto &r : R) {
    before.push_back(evalRef(r));
  }

  for (bool scheduled : {true, false}) {
    ExtractOptions opts;
    opts.scheduled = scheduled;
    Factory G;
    auto out = ean::ean<int>(R, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                             ean::Budget::unbounded(), G, nullptr, opts);
    ASSERT_EQ(out.size(), R.size()) << "scheduled=" << scheduled;
    for (std::size_t i = 0; i < R.size(); ++i) {
      EXPECT_TRUE(evalRef(out[i]) == before[i])
          << "scheduled=" << scheduled << " root " << i;
    }
  }
}

} // namespace

// EAN M5 tests: Kleene sliding  (a·b)*·a = a·(b·a)*  in the Star phase.

#include "Dataflow/APA/EAN/EAN.h"
#include "Dataflow/APA/EAN/Export.h"
#include "Dataflow/APA/EAN/Import.h"
#include "Dataflow/APA/EAN/Star.h"

#include "BoolKleene.h"
#include "EanGraphEval.h"

#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace lotus_test_ean;
namespace ean = elimination::ean;
using ean::CostModel;
using ean::ExtractOptions;
using ean::Id;
using ean::Law;
using ean::LawProfile;
using Factory = elimination::PathExprFactory<int>;
using Ref = Factory::Ref;

// (a·b)*·a
Ref slideInput(Factory &F) {
  Ref a = F.atom(0), b = F.atom(1);
  return F.concat(F.star(F.concat(a, b)), a);
}

std::uint64_t g_seed = 0x571DE;
std::uint32_t rnd() {
  g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
  return static_cast<std::uint32_t>(g_seed >> 33);
}
Ref randExpr(Factory &F, int depth) {
  if (depth <= 0 || (rnd() % 3 == 0)) return F.atom(static_cast<int>(rnd() % 4));
  switch (rnd() % 4) {
  case 0: return F.unite(randExpr(F, depth - 1), randExpr(F, depth - 1));
  case 1: return F.concat(randExpr(F, depth - 1), randExpr(F, depth - 1));
  case 2: return F.star(randExpr(F, depth - 1));
  default: return F.atom(static_cast<int>(rnd() % 4));
  }
}

} // namespace

TEST(EanStar, SlideAppearsAndPreservesSemantics) {
  Factory F;
  Ref r = slideInput(F);
  const Mat before = evalRef(r);

  auto imp = ean::importCanonical<int>({r});
  ASSERT_EQ(imp.g[imp.roots[0]].nodes.size(), 1u);
  const std::size_t changed = ean::slideRound(imp.g, LawProfile::kleeneAlgebra());

  EXPECT_GT(changed, 0u);
  EXPECT_GE(imp.g[imp.roots[0]].nodes.size(), 2u); // slid form added
  EXPECT_TRUE(allClassesConsistent(imp.g, imp.atoms));

  Factory G;
  auto out = ean::exportBatch<int>(imp, G);
  EXPECT_TRUE(evalRef(out[0]) == before);
}

TEST(EanStar, LawProfileGatesSliding) {
  Factory F;
  Ref r = slideInput(F);
  auto imp = ean::importCanonical<int>({r});

  LawProfile noSlide = LawProfile::kleeneAlgebra();
  noSlide.disable(Law::Sliding);
  EXPECT_EQ(ean::slideRound(imp.g, noSlide), 0u);
  EXPECT_EQ(imp.g[imp.roots[0]].nodes.size(), 1u);

  EXPECT_GT(ean::slideRound(imp.g, LawProfile::kleeneAlgebra()), 0u);
  EXPECT_GE(imp.g[imp.roots[0]].nodes.size(), 2u);
}

TEST(EanStar, EndToEndBothPlateauModes) {
  for (ExtractOptions::PlateauCost mode :
       {ExtractOptions::PlateauCost::Tree, ExtractOptions::PlateauCost::Dag}) {
    Factory F;
    Ref r = slideInput(F);
    const Mat before = evalRef(r);

    ExtractOptions opts;
    opts.plateauMode = mode;
    Factory G;
    auto out = ean::ean<int>({r}, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                             ean::Budget::unbounded(), G, nullptr, opts);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(evalRef(out[0]) == before);
  }
}

TEST(EanStar, RandomizedDifferentialWithStars) {
  for (int t = 0; t < 400; ++t) {
    Factory F;
    std::vector<Ref> R;
    std::vector<Mat> before;
    const int k = 1 + static_cast<int>(rnd() % 3);
    for (int i = 0; i < k; ++i) {
      Ref e = randExpr(F, 4);
      R.push_back(e);
      before.push_back(evalRef(e));
    }
    ean::Budget b = ean::Budget::unbounded();
    b.roundLimit = 300;

    Factory G;
    auto out = ean::ean<int>(R, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                             b, G);
    ASSERT_EQ(out.size(), R.size()) << "trial " << t;
    for (std::size_t i = 0; i < R.size(); ++i) {
      EXPECT_TRUE(evalRef(out[i]) == before[i]) << "trial " << t << " root " << i;
    }
  }
}

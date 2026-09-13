// EAN M3 tests: the top-level budgeted saturation pipeline `ean()`.
//
// Checks: (1) it reduces the retained DAG and cost on a factorable batch while
// preserving the boolean-matrix interpretation; (2) budgets stop it early yet
// still yield a valid (anytime) result; (3) an empty law profile is a
// semantics-preserving no-op; (4) it never changes semantics on random batches
// and terminates.

#include "Dataflow/APA/EAN/EAN.h"

#include "BoolKleene.h"

#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace lotus_test_ean;
namespace ean = elimination::ean;
using ean::Budget;
using ean::CostModel;
using ean::LawProfile;
using ean::SaturationStats;
using ean::StopReason;
using Factory = elimination::PathExprFactory<int>;
using Ref = Factory::Ref;

// Count distinct hash-consed nodes reachable from a Ref.
void collectUnique(const Ref &e, std::unordered_set<const void *> &seen) {
  if (!e || !seen.insert(e.get()).second) return;
  if (e->L) collectUnique(e->L, seen);
  if (e->R) collectUnique(e->R, seen);
}
std::size_t countUnique(const Ref &e) {
  std::unordered_set<const void *> seen;
  collectUnique(e, seen);
  return seen.size();
}

Ref seq4(Factory &F, Ref w, Ref x, Ref y, Ref z) {
  return F.concat(F.concat(F.concat(w, x), y), z);
}

// (a·b·c·d) ⊕ (a·b·c·e) ⊕ (a·b·c·f): a long prefix shared across 3 branches.
Ref threeBranch(Factory &F) {
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2);
  Ref d = F.atom(3), e = F.atom(4), f = F.atom(5);
  return F.unite(F.unite(seq4(F, a, b, c, d), seq4(F, a, b, c, e)),
                 seq4(F, a, b, c, f));
}

std::uint64_t g_seed = 0xBEEF77;
std::uint32_t rnd() {
  g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
  return static_cast<std::uint32_t>(g_seed >> 33);
}
Ref randExpr(Factory &F, int depth) {
  if (depth <= 0 || (rnd() % 3 == 0)) return F.atom(static_cast<int>(rnd() % 5));
  switch (rnd() % 4) {
  case 0: return F.unite(randExpr(F, depth - 1), randExpr(F, depth - 1));
  case 1: return F.concat(randExpr(F, depth - 1), randExpr(F, depth - 1));
  case 2: return F.star(randExpr(F, depth - 1));
  default: return F.atom(static_cast<int>(rnd() % 5));
  }
}

} // namespace

TEST(EanSaturate, ReducesNodesAndPreservesSemantics) {
  Factory F;
  Ref r = threeBranch(F);
  const Mat before = evalRef(r);
  const std::size_t origUnique = countUnique(r);

  Factory G;
  SaturationStats st;
  auto out = ean::ean<int>({r}, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                           Budget::unbounded(), G, &st);

  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(evalRef(out[0]) == before);
  EXPECT_LT(countUnique(out[0]), origUnique);
  EXPECT_LT(st.finalCost, st.initCost);
  EXPECT_GT(st.rounds, 0u);
}

TEST(EanSaturate, RoundBudgetStopsEarlyButValid) {
  Factory F;
  Ref r = threeBranch(F);
  const Mat before = evalRef(r);

  Budget b;
  b.roundLimit = 1;
  Factory G;
  SaturationStats st;
  auto out = ean::ean<int>({r}, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                           b, G, &st);

  EXPECT_EQ(st.stop, StopReason::RoundLimit);
  EXPECT_EQ(st.rounds, 1u);
  EXPECT_TRUE(evalRef(out[0]) == before); // anytime: still a valid equivalent
}

TEST(EanSaturate, NodeBudgetStopsEarlyButValid) {
  Factory F;
  Ref r = threeBranch(F);
  const Mat before = evalRef(r);

  Budget b;
  b.nodeLimit = 1; // any change pushes totalSize past this immediately
  Factory G;
  SaturationStats st;
  auto out = ean::ean<int>({r}, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                           b, G, &st);

  EXPECT_EQ(st.stop, StopReason::NodeLimit);
  EXPECT_TRUE(evalRef(out[0]) == before);
}

TEST(EanSaturate, EmptyProfileIsSemanticsPreservingNoOp) {
  Factory F;
  Ref r = threeBranch(F);
  const Mat before = evalRef(r);

  Factory G;
  SaturationStats st;
  auto out = ean::ean<int>({r}, LawProfile::none(), CostModel::uniform(),
                           Budget::unbounded(), G, &st);

  EXPECT_EQ(st.rounds, 0u);
  EXPECT_EQ(st.stop, StopReason::Saturated);
  EXPECT_TRUE(evalRef(out[0]) == before);
}

TEST(EanSaturate, RandomizedDifferentialBatch) {
  for (int t = 0; t < 400; ++t) {
    Factory F;
    std::vector<Ref> R;
    std::vector<Mat> before;
    const int k = 1 + static_cast<int>(rnd() % 3); // 1..3 roots
    for (int i = 0; i < k; ++i) {
      Ref e = randExpr(F, 4);
      R.push_back(e);
      before.push_back(evalRef(e));
    }
    Budget b = Budget::unbounded();
    b.roundLimit = 200; // safety net against pathological growth

    Factory G;
    auto out = ean::ean<int>(R, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                             b, G);
    ASSERT_EQ(out.size(), R.size()) << "trial " << t;
    for (std::size_t i = 0; i < R.size(); ++i) {
      EXPECT_TRUE(evalRef(out[i]) == before[i]) << "trial " << t << " root " << i;
    }
  }
}

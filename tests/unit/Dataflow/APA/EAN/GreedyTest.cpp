// Greedy one-pass simplifier tests (paper's "Greedy" configuration).
//
// greedySimplify applies deterministic prefix factorization on the factory DAG.
// We check (a) it preserves semantics under the boolean-matrix Kleene oracle
// (prefix factorization = left distributivity, universally sound), and (b) it
// actually reduces the retained DAG on shared-prefix batches.

#include "Dataflow/APA/EAN/DagStats.h"
#include "Dataflow/APA/EAN/Greedy.h"

#include "BoolKleene.h"

#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace lotus_test_ean;
using Factory = elimination::PathExprFactory<int>;
using Ref = Factory::Ref;

Ref seqOf(Factory &F, const std::vector<Ref> &xs) {
  Ref acc = xs.front();
  for (std::size_t i = 1; i < xs.size(); ++i) acc = F.concat(acc, xs[i]);
  return acc;
}

std::size_t nodes(const std::vector<Ref> &R) {
  return elimination::ean::computeDagStats<int>(R).uniqueNodes;
}

// (a·b·c) ⊕ (a·b·d): shared prefix a·b must be factored out.
TEST(Greedy, FactorsSharedPrefixAndPreservesSemantics) {
  Factory F;
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2), d = F.atom(3);
  std::vector<Ref> R = {F.unite(seqOf(F, {a, b, c}), seqOf(F, {a, b, d}))};
  const Mat want = evalRef(R[0]);

  auto out = elimination::greedySimplify<int>(R, F);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(evalRef(out[0]) == want);          // semantics preserved
  EXPECT_LT(nodes(out), nodes(R));               // strictly smaller DAG
}

// Cross-root shared prefix: two roots sharing a·b, factored per root, and the
// factory hash-conses the shared factor across roots.
TEST(Greedy, CrossRootSharingPreserved) {
  Factory F;
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2), d = F.atom(3), e = F.atom(4);
  std::vector<Ref> R = {
      F.unite(seqOf(F, {a, b, c}), seqOf(F, {a, b, d})),
      F.unite(seqOf(F, {a, b, d}), seqOf(F, {a, b, e}))};
  std::vector<Mat> want = {evalRef(R[0]), evalRef(R[1])};

  auto out = elimination::greedySimplify<int>(R, F);
  ASSERT_EQ(out.size(), 2u);
  for (std::size_t i = 0; i < 2; ++i)
    EXPECT_TRUE(evalRef(out[i]) == want[i]) << "root " << i;
  EXPECT_LE(nodes(out), nodes(R));
}

// Idempotent: greedySimplify of an already-simplified batch is a fixpoint.
TEST(Greedy, Idempotent) {
  Factory F;
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2);
  std::vector<Ref> R = {F.unite(seqOf(F, {a, b}), seqOf(F, {a, c}))};
  auto o1 = elimination::greedySimplify<int>(R, F);
  auto o2 = elimination::greedySimplify<int>(o1, F);
  ASSERT_EQ(o1.size(), o2.size());
  EXPECT_EQ(nodes(o1), nodes(o2));
  EXPECT_TRUE(evalRef(o1[0]) == evalRef(o2[0]));
}

// Randomized differential: greedySimplify never changes the interpretation.
std::uint64_t g_seed = 0x51EED;
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

TEST(Greedy, RandomizedDifferential) {
  for (int t = 0; t < 500; ++t) {
    Factory F;
    std::vector<Ref> R;
    std::vector<Mat> want;
    const int k = 1 + static_cast<int>(rnd() % 3);
    for (int i = 0; i < k; ++i) {
      Ref e = randExpr(F, 5);
      R.push_back(e);
      want.push_back(evalRef(e));
    }
    auto out = elimination::greedySimplify<int>(R, F);
    ASSERT_EQ(out.size(), R.size()) << "trial " << t;
    for (std::size_t i = 0; i < R.size(); ++i)
      EXPECT_TRUE(evalRef(out[i]) == want[i]) << "trial " << t << " root " << i;
    EXPECT_LE(nodes(out), nodes(R)) << "trial " << t;
  }
}

} // namespace

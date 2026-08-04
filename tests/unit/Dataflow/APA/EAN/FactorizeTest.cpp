// EAN M2 factorization tests.
//
// Correctness is checked two ways:
//  1. Differential: import -> factorize -> export, and the boolean-matrix
//     Kleene interpretation is unchanged from the original expression.
//  2. E-graph consistency: EVERY e-node in EVERY class evaluates to the same
//     matrix as its class representative — i.e. factorization never united a
//     semantically-different node into a class. This directly validates the
//     factored e-nodes, not just whichever one export happens to pick.
// Plus structural checks (a factored form actually appeared) and law gating.

#include "Dataflow/APA/EAN/Export.h"
#include "Dataflow/APA/EAN/Factorize.h"
#include "Dataflow/APA/EAN/Import.h"

#include "BoolKleene.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace lotus_test_ean;
namespace ean = elimination::ean;
using ean::Graph;
using ean::Id;
using ean::LawProfile;
using ean::Law;
using ean::PathLang;
using Factory = elimination::PathExprFactory<int>;
using Ref = Factory::Ref;

// ---- graph-side evaluator (front representative) + consistency check --------

Mat evalClass(Graph &g, const ean::AtomTable<int> &atoms, Id c,
              std::unordered_map<std::uint32_t, Mat> &memo);

Mat evalNode(Graph &g, const ean::AtomTable<int> &atoms, const PathLang &n,
             std::unordered_map<std::uint32_t, Mat> &memo) {
  if (ean::isZero(n)) return zeroM();
  if (ean::isOne(n)) return oneM();
  if (ean::isAtom(n)) return gen(*atoms.atom(ean::parseAtomId(n.op()))->Transfer);
  if (ean::isStar(n)) return closureM(evalClass(g, atoms, n.children()[0], memo));
  if (ean::isJoin(n)) {
    Mat m = zeroM();
    for (Id ch : n.children()) m = orM(m, evalClass(g, atoms, ch, memo));
    return m;
  }
  // seq
  Mat m = oneM();
  for (Id ch : n.children()) m = mulM(m, evalClass(g, atoms, ch, memo));
  return m;
}

Mat evalClass(Graph &g, const ean::AtomTable<int> &atoms, Id c,
              std::unordered_map<std::uint32_t, Mat> &memo) {
  c = g.find(c);
  auto it = memo.find(c.value());
  if (it != memo.end()) return it->second;
  Mat m = evalNode(g, atoms, g[c].nodes.front(), memo);
  memo[c.value()] = m;
  return m;
}

// Every e-node in every class must equal its class representative.
bool allClassesConsistent(Graph &g, const ean::AtomTable<int> &atoms) {
  std::unordered_map<std::uint32_t, Mat> memo;
  for (Id c : g.classIds()) {
    c = g.find(c);
    (void)evalClass(g, atoms, c, memo); // representative value
  }
  for (Id c : g.classIds()) {
    c = g.find(c);
    const Mat ref = memo[c.value()];
    for (const PathLang &n : g[c].nodes) {
      if (evalNode(g, atoms, n, memo) != ref) return false;
    }
  }
  return true;
}

// deterministic LCG for reproducible random expressions
std::uint64_t g_seed = 0xC0FFEE;
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

Ref seq3(Factory &F, Ref x, Ref y, Ref z) {
  return F.concat(F.concat(x, y), z);
}

} // namespace

// (a·b·c) ⊕ (a·b·d)  →  a·b·(c⊕d) added; semantics preserved.
TEST(EanFactorize, PrefixFactorAppearsAndPreservesSemantics) {
  Factory F;
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2), d = F.atom(3);
  Ref r = F.unite(seq3(F, a, b, c), seq3(F, a, b, d));
  const Mat before = evalRef(r);

  auto imp = ean::importCanonical<int>({r});
  ASSERT_EQ(imp.g[imp.roots[0]].nodes.size(), 1u); // one JOIN node after import
  ean::factorizeToFixpoint(imp.g, LawProfile::kleeneAlgebra());

  EXPECT_GE(imp.g[imp.roots[0]].nodes.size(), 2u) << "no factored form added";
  EXPECT_TRUE(allClassesConsistent(imp.g, imp.atoms));

  Factory G;
  auto out = ean::exportBatch<int>(imp, G);
  EXPECT_TRUE(evalRef(out[0]) == before);
}

// Partial grouping: (a·b·c) ⊕ (a·b·d) ⊕ x  →  (a·b·(c⊕d)) ⊕ x.
TEST(EanFactorize, PartialGroupingKeepsNonParticipant) {
  Factory F;
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2), d = F.atom(3), x = F.atom(4);
  Ref r = F.unite(F.unite(seq3(F, a, b, c), seq3(F, a, b, d)), x);
  const Mat before = evalRef(r);

  auto imp = ean::importCanonical<int>({r});
  ean::factorizeToFixpoint(imp.g, LawProfile::kleeneAlgebra());

  EXPECT_GE(imp.g[imp.roots[0]].nodes.size(), 2u);
  EXPECT_TRUE(allClassesConsistent(imp.g, imp.atoms));
  Factory G;
  auto out = ean::exportBatch<int>(imp, G);
  EXPECT_TRUE(evalRef(out[0]) == before);
}

// Suffix: (c·a) ⊕ (d·a)  →  (c⊕d)·a.
TEST(EanFactorize, SuffixFactorPreservesSemantics) {
  Factory F;
  Ref a = F.atom(0), c = F.atom(2), d = F.atom(3);
  Ref r = F.unite(F.concat(c, a), F.concat(d, a));
  const Mat before = evalRef(r);

  auto imp = ean::importCanonical<int>({r});
  ean::factorizeToFixpoint(imp.g, LawProfile::kleeneAlgebra());

  EXPECT_GE(imp.g[imp.roots[0]].nodes.size(), 2u);
  EXPECT_TRUE(allClassesConsistent(imp.g, imp.atoms));
  Factory G;
  auto out = ean::exportBatch<int>(imp, G);
  EXPECT_TRUE(evalRef(out[0]) == before);
}

// Disabling LeftDistributive suppresses prefix factorization.
TEST(EanFactorize, LawProfileGatesPrefix) {
  Factory F;
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2), d = F.atom(3);
  Ref r = F.unite(seq3(F, a, b, c), seq3(F, a, b, d)); // only a prefix factor exists

  auto imp = ean::importCanonical<int>({r});
  // Right only: no common suffix (c != d) -> nothing fires.
  LawProfile rightOnly;
  rightOnly.enable(Law::RightDistributive);
  EXPECT_EQ(ean::factorizeRound(imp.g, rightOnly), 0u);
  EXPECT_EQ(imp.g[imp.roots[0]].nodes.size(), 1u);

  // Enable left -> prefix factor now appears.
  LawProfile leftOnly;
  leftOnly.enable(Law::LeftDistributive);
  EXPECT_GT(ean::factorizeRound(imp.g, leftOnly), 0u);
  EXPECT_GE(imp.g[imp.roots[0]].nodes.size(), 2u);
}

// Factorization never changes the interpretation, on random expressions.
TEST(EanFactorize, RandomizedDifferential) {
  for (int t = 0; t < 500; ++t) {
    Factory F;
    Ref e = randExpr(F, 4);
    const Mat before = evalRef(e);

    auto imp = ean::importCanonical<int>({e});
    ean::factorizeToFixpoint(imp.g, LawProfile::kleeneAlgebra());

    EXPECT_TRUE(allClassesConsistent(imp.g, imp.atoms)) << "trial " << t;
    Factory G;
    auto out = ean::exportBatch<int>(imp, G);
    EXPECT_TRUE(evalRef(out[0]) == before) << "trial " << t;
  }
}

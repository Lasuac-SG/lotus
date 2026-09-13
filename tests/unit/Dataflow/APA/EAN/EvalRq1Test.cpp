// EAN evaluation — RQ1 (correctness + IR quality) on synthetic path-expression
// families (paper Tables IV-synthetic and VI, LLVM-free).
//
// For each family we build a summary-root batch, run EAN, and:
//   * assert semantic parity via the boolean-matrix Kleene oracle (which
//     satisfies every law, so the full profile is sound) — RQ1 correctness;
//   * report the retained-DAG reduction ratios (unique nodes, edges, concats,
//     stars, sharing) EAN/Default — RQ1 IR quality.
// A randomized differential over random law-profile subsets checks that EAN
// preserves semantics under any declared profile.

#include "Dataflow/APA/EAN/DagStats.h"
#include "Dataflow/APA/EAN/EAN.h"

#include "BoolKleene.h"

#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace lotus_test_ean;
namespace ean = elimination::ean;
using ean::Budget;
using ean::CostModel;
using ean::DagStats;
using ean::ExtractOptions;
using ean::LawProfile;
using Factory = elimination::PathExprFactory<int>;
using Ref = Factory::Ref;

Ref seqOf(Factory &F, const std::vector<Ref> &xs) {
  Ref acc = xs.front();
  for (std::size_t i = 1; i < xs.size(); ++i) acc = F.concat(acc, xs[i]);
  return acc;
}
Ref unionOf(Factory &F, const std::vector<Ref> &xs) {
  Ref acc = xs.front();
  for (std::size_t i = 1; i < xs.size(); ++i) acc = F.unite(acc, xs[i]);
  return acc;
}

// ⊕_{i<k} (p0·…·p_{L-1} · c_i): k branches share an L-atom prefix.
std::vector<Ref> repeatedPrefix(Factory &F, int k, int L) {
  std::vector<Ref> pre;
  for (int i = 0; i < L; ++i) pre.push_back(F.atom(i));
  std::vector<Ref> branches;
  for (int i = 0; i < k; ++i) {
    std::vector<Ref> s = pre;
    s.push_back(F.atom(100 + i));
    branches.push_back(seqOf(F, s));
  }
  return {unionOf(F, branches)};
}

// ⊕_{i<k} (c_i · s0·…·s_{L-1}): k branches share an L-atom suffix.
std::vector<Ref> repeatedSuffix(Factory &F, int k, int L) {
  std::vector<Ref> suf;
  for (int i = 0; i < L; ++i) suf.push_back(F.atom(i));
  std::vector<Ref> branches;
  for (int i = 0; i < k; ++i) {
    std::vector<Ref> s;
    s.push_back(F.atom(100 + i));
    for (Ref x : suf) s.push_back(x);
    branches.push_back(seqOf(F, s));
  }
  return {unionOf(F, branches)};
}

// m roots each = sharedQ · atom_j, where sharedQ is a common factorable block.
std::vector<Ref> crossRootReuse(Factory &F, int m) {
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2), d = F.atom(3);
  Ref q = F.unite(seqOf(F, {a, b, c}), seqOf(F, {a, b, d})); // (a·b·c)⊕(a·b·d)
  std::vector<Ref> roots;
  for (int j = 0; j < m; ++j) roots.push_back(F.concat(q, F.atom(200 + j)));
  return roots;
}

// (a·b)*·a  plus a nested loop — exercises sliding and star handling.
std::vector<Ref> loopFamily(Factory &F) {
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2);
  Ref r1 = F.concat(F.star(F.concat(a, b)), a);            // (a·b)*·a
  Ref r2 = F.concat(F.star(F.concat(a, F.star(F.concat(b, c)))), a);
  return {r1, r2};
}

void report(const char *name, const DagStats &d, const DagStats &e,
            const ean::SaturationStats &s) {
  auto ratio = [](std::size_t before, std::size_t after) {
    return before ? static_cast<double>(after) / static_cast<double>(before) : 1.0;
  };
  std::printf(
      "[RQ1] %-22s nodes %3zu->%3zu (%.2fx) edges %3zu->%3zu concat %2zu->%2zu "
      "star %2zu->%2zu sharing %.2f->%.2f | rounds=%zu peak=%zu\n",
      name, d.uniqueNodes, e.uniqueNodes, ratio(d.uniqueNodes, e.uniqueNodes),
      d.uniqueEdges, e.uniqueEdges, d.concats, e.concats, d.stars, e.stars,
      d.sharing(), e.sharing(), s.rounds, s.peakNodes);
}

// Run EAN with the full (Kleene) profile and check semantic parity + report.
void runFamily(const char *name, const std::vector<Ref> &R) {
  DagStats before = ean::computeDagStats<int>(R);
  std::vector<Mat> want;
  for (const auto &r : R) want.push_back(evalRef(r));

  Factory G;
  ean::SaturationStats stats;
  auto out = ean::ean<int>(R, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                           Budget::unbounded(), G, &stats);
  ASSERT_EQ(out.size(), R.size()) << name;
  for (std::size_t i = 0; i < R.size(); ++i) {
    EXPECT_TRUE(evalRef(out[i]) == want[i]) << name << " root " << i;
  }
  DagStats after = ean::computeDagStats<int>(out);
  report(name, before, after, stats);
  // EAN must never inflate the retained node count on these families.
  EXPECT_LE(after.uniqueNodes, before.uniqueNodes) << name;
}

std::uint64_t g_seed = 0xE7A1;
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

TEST(EanEvalRq1, SyntheticFamiliesPreserveSemanticsAndReduce) {
  { Factory F; runFamily("RepeatedPrefix(8,4)", repeatedPrefix(F, 8, 4)); }
  { Factory F; runFamily("RepeatedSuffix(8,4)", repeatedSuffix(F, 8, 4)); }
  { Factory F; runFamily("CrossRootReuse(6)", crossRootReuse(F, 6)); }
  { Factory F; runFamily("LoopFamily", loopFamily(F)); }
}

// Reuse-aware extraction (K=3) vs tree extraction (K=0) on a sharing-heavy
// batch: reuse-aware must not retain more nodes.
TEST(EanEvalRq1, ReuseAwareVsTreeExtraction) {
  Factory F;
  auto R = crossRootReuse(F, 6);

  Factory G0;
  ExtractOptions tree;
  tree.reuseIters = 0;
  auto treeOut = ean::ean<int>(R, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                               Budget::unbounded(), G0, nullptr, tree);
  Factory G3;
  ExtractOptions reuse;
  reuse.reuseIters = 3;
  auto reuseOut = ean::ean<int>(R, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                                Budget::unbounded(), G3, nullptr, reuse);

  const std::size_t treeNodes = ean::computeDagStats<int>(treeOut).uniqueNodes;
  const std::size_t reuseNodes = ean::computeDagStats<int>(reuseOut).uniqueNodes;
  std::printf("[RQ1] extractor tree=%zu reuse=%zu\n", treeNodes, reuseNodes);
  EXPECT_LE(reuseNodes, treeNodes);
}

// Differential: EAN preserves semantics under randomly chosen law subsets
// (the boolean-matrix algebra satisfies every law, so all subsets are sound).
TEST(EanEvalRq1, RandomLawProfileDifferential) {
  const ean::Law laws[] = {ean::Law::LeftDistributive, ean::Law::RightDistributive,
                           ean::Law::Sliding};
  for (int t = 0; t < 300; ++t) {
    Factory F;
    std::vector<Ref> R;
    std::vector<Mat> want;
    const int k = 1 + static_cast<int>(rnd() % 3);
    for (int i = 0; i < k; ++i) {
      Ref e = randExpr(F, 4);
      R.push_back(e);
      want.push_back(evalRef(e));
    }
    LawProfile L;
    for (ean::Law law : laws)
      if (rnd() & 1) L.enable(law);

    Factory G;
    Budget b = Budget::unbounded();
    b.roundLimit = 200;
    auto out = ean::ean<int>(R, L, CostModel::uniform(), b, G);
    ASSERT_EQ(out.size(), R.size()) << "trial " << t;
    for (std::size_t i = 0; i < R.size(); ++i)
      EXPECT_TRUE(evalRef(out[i]) == want[i]) << "trial " << t << " root " << i;
  }
}

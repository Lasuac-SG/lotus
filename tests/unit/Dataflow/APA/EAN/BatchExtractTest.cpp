// EAN M4 tests: reuse-aware batch extraction (Algorithm 2), the Eq. 5 objective,
// cycle-safe extraction, and the switchable plateau mode.

#include "Dataflow/APA/EAN/BatchExtract.h"
#include "Dataflow/APA/EAN/Canonical.h"
#include "Dataflow/APA/EAN/EAN.h"
#include "Dataflow/APA/EAN/Export.h"
#include "Dataflow/APA/EAN/Factorize.h"
#include "Dataflow/APA/EAN/Import.h"

#include "BoolKleene.h"

#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace lotus_test_ean;
namespace ean = elimination::ean;
using ean::CostModel;
using ean::ExtractOptions;
using ean::Graph;
using ean::Id;
using ean::LawProfile;
using ean::PathLang;
using Factory = elimination::PathExprFactory<int>;
using Ref = Factory::Ref;

void collectUnique(const Ref &e, std::unordered_set<const void *> &seen) {
  if (!e || !seen.insert(e.get()).second) return;
  if (e->L) collectUnique(e->L, seen);
  if (e->R) collectUnique(e->R, seen);
}
std::size_t countUnique(const std::vector<Ref> &roots) {
  std::unordered_set<const void *> seen;
  for (const auto &r : roots) collectUnique(r, seen);
  return seen.size();
}

Ref seq3(Factory &F, Ref x, Ref y, Ref z) {
  return F.concat(F.concat(x, y), z);
}

std::uint64_t g_seed = 0x5A1AD;
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

// dagCost computes Eq. 5 correctly on tiny known graphs.
TEST(EanBatchExtract, DagCostFormula) {
  ExtractOptions o0;
  o0.reuseIters = 0;

  { // single atom: alpha*1 + wAtom*1 = 2
    Factory F;
    auto imp = ean::importCanonical<int>({F.atom(0)});
    double c = ean::reuseAwareExtract(imp.g, imp.roots, CostModel::uniform(), o0).dagCost;
    EXPECT_DOUBLE_EQ(c, 2.0);
  }
  { // a·b: N_unique=3, opTerm=wAtom+wAtom+wSeq=3 -> 1*3+3 = 6
    Factory F;
    auto imp = ean::importCanonical<int>({F.concat(F.atom(0), F.atom(1))});
    double c = ean::reuseAwareExtract(imp.g, imp.roots, CostModel::uniform(), o0).dagCost;
    EXPECT_DOUBLE_EQ(c, 6.0);
  }
  { // a·a with gamma=1: N_unique=2, opTerm=2, C_repeat=(2-1)*wAtom=1 -> 2+2+1 = 5
    Factory F;
    CostModel cm = CostModel::uniform();
    cm.gamma = 1.0;
    auto imp = ean::importCanonical<int>({F.concat(F.atom(0), F.atom(0))});
    double c = ean::reuseAwareExtract(imp.g, imp.roots, cm, o0).dagCost;
    EXPECT_DOUBLE_EQ(c, 5.0);
  }
}

// A cyclic e-graph (star + its unfolding referencing itself) must extract to a
// finite expression and terminate.
TEST(EanBatchExtract, CycleSafeExtractsFiniteRepresentative) {
  Factory F;
  Ref a = F.atom(0);
  Ref starA = F.star(a);
  const Mat before = evalRef(starA);

  auto imp = ean::importCanonical<int>({starA});
  const Id starCls = imp.g.find(imp.roots[0]);
  const PathLang &starNode = imp.g[starCls].nodes.front();
  ASSERT_TRUE(ean::isStar(starNode));
  const Id aCls = imp.g.find(starNode.children()[0]);

  // Add the star unfolding  1 ⊕ a·(a*)  and merge it into the star class,
  // making the e-graph intentionally cyclic (the unfold refers back to a*).
  const Id aStar = ean::canon::seq(imp.g, {aCls, starCls});    // a·(a*)
  const Id unfold =
      ean::canon::join(imp.g, {ean::canon::oneId(imp.g), aStar}); // 1 ⊕ a·(a*)
  imp.g.uniteChecked(starCls, unfold);
  imp.g.rebuild();

  // Extraction must terminate and pick the finite star representative.
  auto res = ean::reuseAwareExtract(imp.g, {imp.g.find(starCls)},
                                    CostModel::uniform(), ExtractOptions{});
  const PathLang &chosen = res.chosen.at(imp.g.find(starCls).value());
  EXPECT_TRUE(ean::isStar(chosen)) << "extractor picked the cyclic unfold";

  Factory G;
  ean::PickFn pick = [&](Id c) -> const PathLang & {
    return res.chosen.at(imp.g.find(c).value());
  };
  Ref out = ean::exportTree<int>(imp.g, imp.atoms, imp.g.find(starCls), G, pick);
  EXPECT_TRUE(evalRef(out) == before);
}

// The reuse loop never yields a worse Eq. 5 cost than the initial tree solution
// (safety net), and export stays semantics-preserving.
TEST(EanBatchExtract, ReuseNeverWorseThanTree) {
  Factory F;
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2), d = F.atom(3);
  Ref p = F.unite(a, b);
  Ref r1 = F.unite(F.concat(p, c), F.concat(p, d));
  Ref r2 = F.concat(p, c);
  std::vector<Ref> R = {r1, r2};
  std::vector<Mat> before = {evalRef(r1), evalRef(r2)};

  auto imp = ean::importCanonical<int>(R);
  ean::factorizeToFixpoint(imp.g, LawProfile::kleeneAlgebra());

  CostModel cm = CostModel::uniform();
  ExtractOptions o0;
  o0.reuseIters = 0;
  ExtractOptions o3;
  o3.reuseIters = 3;
  const double cTree = ean::reuseAwareExtract(imp.g, imp.roots, cm, o0).dagCost;
  const double cReuse = ean::reuseAwareExtract(imp.g, imp.roots, cm, o3).dagCost;
  EXPECT_LE(cReuse, cTree + 1e-9);

  auto res = ean::reuseAwareExtract(imp.g, imp.roots, cm, o3);
  Factory G;
  ean::PickFn pick = [&](Id id) -> const PathLang & {
    return res.chosen.at(imp.g.find(id).value());
  };
  auto out = ean::exportBatch<int>(imp, G, pick);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_TRUE(evalRef(out[0]) == before[0]);
  EXPECT_TRUE(evalRef(out[1]) == before[1]);
}

// Full ean() with reuse-aware extraction reduces nodes and preserves semantics
// in both plateau modes.
TEST(EanBatchExtract, EndToEndBothPlateauModes) {
  for (ExtractOptions::PlateauCost mode :
       {ExtractOptions::PlateauCost::Tree, ExtractOptions::PlateauCost::Dag}) {
    Factory F;
    Ref a = F.atom(0), b = F.atom(1), c = F.atom(2), d = F.atom(3), e = F.atom(4);
    Ref r = F.unite(F.unite(seq3(F, a, b, c), seq3(F, a, b, d)), seq3(F, a, b, e));
    const Mat before = evalRef(r);
    const std::size_t origUnique = countUnique({r});

    ExtractOptions opts;
    opts.plateauMode = mode;
    Factory G;
    auto out = ean::ean<int>({r}, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                             ean::Budget::unbounded(), G, nullptr, opts);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(evalRef(out[0]) == before);
    EXPECT_LE(countUnique(out), origUnique);
  }
}

// Decisive byte-identical guard for the work-queue rewrite of cycleSafeExtract:
// the parent-pointer worklist must compute the SAME least fixpoint (identical
// chosen node and cost per class) as the previous whole-graph pass relaxation.
// A local copy of the naive relaxation is the reference oracle.
namespace {
ean::detail::RelaxState naiveCycleSafe(ean::Graph &g, std::size_t passes) {
  ean::PathCostFn wf(CostModel::uniform());
  auto wNode = [&](const PathLang &n) { return wf.opWeight(n); };
  auto disc = [](std::uint32_t) { return 1.0; };
  ean::detail::RelaxState st;
  const auto ids = g.classIds();
  for (std::size_t pass = 0; pass < passes; ++pass) {
    bool changed = false;
    for (Id c0 : ids) {
      const std::uint32_t cid = g.find(c0).value();
      const auto &cls = g[c0];
      for (const PathLang &n : cls.nodes) {
        double cost = wNode(n);
        bool eligible = true;
        for (Id q : n.children()) {
          const std::uint32_t qid = g.find(q).value();
          const double cc = st.at(qid);
          if (cc == ean::detail::kInf) {
            eligible = false;
            break;
          }
          cost += cc * disc(qid);
        }
        if (eligible && cost < st.at(cid)) {
          st.cost[cid] = cost;
          st.chosen[cid] = n;
          changed = true;
        }
      }
    }
    if (!changed)
      break;
  }
  return st;
}
} // namespace

TEST(EanBatchExtract, WorklistExtractMatchesNaive) {
  ean::PathCostFn wf(CostModel::uniform());
  auto wNode = [&](const PathLang &n) { return wf.opWeight(n); };
  auto disc = [](std::uint32_t) { return 1.0; };

  for (int t = 0; t < 300; ++t) {
    Factory F;
    std::vector<Ref> R;
    const int k = 1 + static_cast<int>(rnd() % 3);
    for (int i = 0; i < k; ++i)
      R.push_back(randExpr(F, 4));

    auto imp = ean::importCanonical<int>(R);
    // Grow alternatives so classes carry multiple nodes (non-trivial extraction).
    ean::factorizeToFixpoint(imp.g, LawProfile::kleeneAlgebra());
    imp.g.rebuild();

    const std::size_t passes = imp.g.numberOfClasses() + 1;
    ean::detail::RelaxState wl =
        ean::detail::cycleSafeExtract(imp.g, wNode, disc, passes);
    ean::detail::RelaxState nv = naiveCycleSafe(imp.g, passes);

    ASSERT_EQ(wl.cost.size(), nv.cost.size()) << "trial " << t;
    for (const auto &kv : nv.cost) {
      auto it = wl.cost.find(kv.first);
      ASSERT_NE(it, wl.cost.end()) << "trial " << t << " class " << kv.first;
      EXPECT_DOUBLE_EQ(it->second, kv.second) << "trial " << t;
      auto cit = wl.chosen.find(kv.first);
      auto nit = nv.chosen.find(kv.first);
      ASSERT_NE(cit, wl.chosen.end());
      ASSERT_NE(nit, nv.chosen.end());
      EXPECT_TRUE(cit->second == nit->second)
          << "trial " << t << " class " << kv.first << ": chosen node differs";
    }
  }
}

TEST(EanBatchExtract, RandomizedDifferential) {
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
    ExtractOptions opts;
    opts.reuseIters = 3;
    opts.plateauMode = (t % 2) ? ExtractOptions::PlateauCost::Dag
                               : ExtractOptions::PlateauCost::Tree;
    ean::Budget b = ean::Budget::unbounded();
    b.roundLimit = 200;

    Factory G;
    auto out = ean::ean<int>(R, LawProfile::kleeneAlgebra(), CostModel::uniform(),
                             b, G, nullptr, opts);
    ASSERT_EQ(out.size(), R.size()) << "trial " << t;
    for (std::size_t i = 0; i < R.size(); ++i) {
      EXPECT_TRUE(evalRef(out[i]) == before[i]) << "trial " << t << " root " << i;
    }
  }
}

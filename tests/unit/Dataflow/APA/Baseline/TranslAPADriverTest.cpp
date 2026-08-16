/**
 * @file TranslAPADriverTest.cpp
 * @brief B2 solver-level differential: fold the SOLVER-produced path-expression
 *        DAGs with the Gen/Kill semiring and require the result to match the
 *        framework's own generic interpreter on every node — the paper's
 *        "same set of dataflow facts" guarantee, checked over real elimination
 *        output including Star (loops) and Union (branches).
 */

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Baseline/TranslAPA/AtomTranslator.h"
#include "Dataflow/APA/Baseline/TranslAPA/Driver.h"

#include <set>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

// Gen/Kill "gen-on-visit" problem (same shape as EliminationTest's): visiting a
// node gens its own id, kills nothing. Separable and distributive, so the
// mechanical translator recovers ({id}, {}) per atom.
struct TestDomain {
  using n_t = int;
  using fact_t = std::set<int>;
  using transfer_t = int;
};

class GenProblem final : public elimination::IntraEliminationProblem<TestDomain> {
public:
  GenProblem(int Entry, std::unordered_map<int, std::vector<int>> Succs)
      : Entry(Entry), Succs(std::move(Succs)) {}

  std::vector<int> nodes() const override {
    std::vector<int> Ns;
    Ns.reserve(Succs.size());
    for (const auto &It : Succs) {
      Ns.push_back(It.first);
    }
    return Ns;
  }
  int entry() const override { return Entry; }
  std::vector<int> succs(int Node) const override {
    auto It = Succs.find(Node);
    return It == Succs.end() ? std::vector<int>{} : It->second;
  }
  int edgeTransfer(int, int Dst) const override { return Dst; }
  fact_t applyTransfer(const int &T, const fact_t &In) const override {
    fact_t Out = In;
    Out.insert(T);
    return Out;
  }
  fact_t meet(const fact_t &A, const fact_t &B) const override {
    fact_t Out = A;
    Out.insert(B.begin(), B.end());
    return Out;
  }
  bool equal_to(const fact_t &A, const fact_t &B) const override {
    return A == B;
  }
  fact_t meetIdentity() const override { return {}; }
  fact_t initialFact() const override { return {}; }
  std::size_t maxStarIterations() const override { return 1000; }

private:
  int Entry;
  std::unordered_map<int, std::vector<int>> Succs;
};

// Run the solver (generic IN), then fold the produced DAGs with the Gen/Kill
// semiring and assert node-by-node agreement.
void expectTranslApaMatchesGeneric(
    int Entry, std::unordered_map<int, std::vector<int>> Succs) {
  GenProblem Problem(Entry, Succs);
  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  auto Generic = Solver.getResults(); // snapshot generic IN + ExprTo

  // Universe = all node ids (every gen'd fact is a node id).
  std::vector<int> Universe;
  for (const auto &It : Succs) {
    Universe.push_back(It.first);
  }
  elimination::translapa::GenKillAtomTranslator<int, std::set<int>> Tr(
      Universe,
      [&Problem](const int &T, const std::set<int> &In) {
        return Problem.applyTransfer(T, In);
      });

  auto TranslApa = Generic; // copy: keeps ExprTo, we overwrite IN
  elimination::translapa::foldFillGenKill<TestDomain>(Problem, TranslApa, Tr);

  for (const int N : Universe) {
    const auto *G = Generic.tryIN(N);
    const auto *T = TranslApa.tryIN(N);
    ASSERT_EQ(G != nullptr, T != nullptr) << "node " << N;
    if (G != nullptr) {
      EXPECT_EQ(*G, *T) << "mismatch at node " << N;
    }
  }
}

TEST(TranslAPADriver, LoopMatchesGeneric) {
  // 0 -> 1 -> 2 -> 3 with back edge 2 -> 1 (Star over {1,2}).
  expectTranslApaMatchesGeneric(
      0, {{0, {1}}, {1, {2}}, {2, {1, 3}}, {3, {}}});
}

TEST(TranslAPADriver, BranchMatchesGeneric) {
  // Diamond 0 -> {1,2} -> 3 (Union).
  expectTranslApaMatchesGeneric(0, {{0, {1, 2}}, {1, {3}}, {2, {3}}, {3, {}}});
}

TEST(TranslAPADriver, SelfLoopMatchesGeneric) {
  expectTranslApaMatchesGeneric(0, {{0, {0}}});
}

TEST(TranslAPADriver, NestedLoopMatchesGeneric) {
  // Outer loop 1..4 with inner loop 2..3.
  expectTranslApaMatchesGeneric(0, {{0, {1}},
                                    {1, {2}},
                                    {2, {3}},
                                    {3, {2, 4}},
                                    {4, {1, 5}},
                                    {5, {}}});
}

// Composition (EAN ⊕ TranslAPA): run the solver with EAN enabled so the
// front-end rewrites Results.ExprTo(N) to the equality-saturated (reduced) DAG,
// then fold THAT DAG with the closed-form Gen/Kill semiring. The result must
// still match the generic interpreter on the raw DAG — this is the correctness
// half of the "two orthogonal levers compose" experiment: EAN shrinks the graph
// (safe-minimal laws preserve semantics) and TranslAPA folds it in closed form,
// so folding the smaller graph yields the same facts.
void expectComposedMatchesGeneric(
    int Entry, std::unordered_map<int, std::vector<int>> Succs) {
  GenProblem Problem(Entry, Succs);

  // Reference: generic interpreter on the raw DAG (no EAN).
  elimination::IntraEliminationSolver<TestDomain> Generic(Problem);
  Generic.solve();
  auto GenericRes = Generic.getResults();

  // Composed: EAN rewrites ExprTo to the reduced DAG; InterpMemo makes the
  // post-pass skip its (discarded) generic eval. We then fold the reduced DAG.
  elimination::EliminationOptions Opts;
  Opts.EnableEAN = true;      // default EANLaws = safe-minimal (universally sound)
  Opts.InterpMemo = true;     // fill IN by folding, not by the generic eval
  elimination::IntraEliminationSolver<TestDomain> EanSolver(Problem, Opts);
  EanSolver.solve();
  auto Composed = EanSolver.getResults();

  std::vector<int> Universe;
  for (const auto &It : Succs) {
    Universe.push_back(It.first);
  }
  elimination::translapa::GenKillAtomTranslator<int, std::set<int>> Tr(
      Universe,
      [&Problem](const int &T, const std::set<int> &In) {
        return Problem.applyTransfer(T, In);
      });
  elimination::translapa::foldFillGenKill<TestDomain>(Problem, Composed, Tr);

  for (const int N : Universe) {
    const auto *G = GenericRes.tryIN(N);
    const auto *C = Composed.tryIN(N);
    ASSERT_EQ(G != nullptr, C != nullptr) << "node " << N;
    if (G != nullptr) {
      EXPECT_EQ(*G, *C) << "composed mismatch at node " << N;
    }
  }
}

TEST(TranslAPADriver, ComposedLoopMatchesGeneric) {
  expectComposedMatchesGeneric(0, {{0, {1}}, {1, {2}}, {2, {1, 3}}, {3, {}}});
}

TEST(TranslAPADriver, ComposedBranchMatchesGeneric) {
  expectComposedMatchesGeneric(0, {{0, {1, 2}}, {1, {3}}, {2, {3}}, {3, {}}});
}

TEST(TranslAPADriver, ComposedNestedLoopMatchesGeneric) {
  expectComposedMatchesGeneric(0, {{0, {1}},
                                   {1, {2}},
                                   {2, {3}},
                                   {3, {2, 4}},
                                   {4, {1, 5}},
                                   {5, {}}});
}

} // namespace

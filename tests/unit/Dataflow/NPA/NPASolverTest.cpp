#include "Dataflow/NPA/Domains/BitSetDomain.h"
#include "Dataflow/NPA/Domains/GenKillDomain.h"
#include "Dataflow/NPA/Domains/TaintDomain.h"
#include "Dataflow/NPA/Domains/TransformerSummary.h"
#include "Dataflow/NPA/NPA.h"
#include "Dataflow/NPA/Solver/Newton/Linear/Tensor/TensorProductLift.h"

#include <algorithm>
#include <set>
#include <unordered_map>

#include <gtest/gtest.h>

namespace {

struct BoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = true;
  static constexpr bool sparse_npa_zero_left_annihilator = true;
  static constexpr bool sparse_npa_zero_right_annihilator = true;

  static value_type zero() { return false; }
  static value_type one() { return true; }

  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
};

struct BothProjectBoolSemiring : BoolSemiring {
  static value_type project(value_type value) { return value; }
  static value_type projectT(value_type) { return false; }
};

struct SparseProjectedBoolSemiring : BoolSemiring {
  static constexpr bool project_newton_safe = true;
  static value_type project(value_type value) { return value; }
};

struct NonDefaultValue {
  explicit NonDefaultValue(bool initial) : value(initial) {}
  bool value;
};

struct NonDefaultSemiring {
  using value_type = NonDefaultValue;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = true;

  static value_type zero() { return NonDefaultValue(false); }
  static value_type one() { return NonDefaultValue(true); }
  static bool equal(const value_type &lhs, const value_type &rhs) {
    return lhs.value == rhs.value;
  }
  static value_type combine(const value_type &lhs, const value_type &rhs) {
    return NonDefaultValue(lhs.value || rhs.value);
  }
  static value_type extend(const value_type &lhs, const value_type &rhs) {
    return NonDefaultValue(lhs.value && rhs.value);
  }
  static value_type extend_lin(const value_type &lhs, const value_type &rhs) {
    return extend(lhs, rhs);
  }
  static value_type ndetCombine(const value_type &lhs, const value_type &rhs) {
    return combine(lhs, rhs);
  }
  static value_type condCombine(test_type condition,
                                const value_type &then_value,
                                const value_type &else_value) {
    return condition ? then_value : else_value;
  }
  static value_type subtract(const value_type &lhs, const value_type &rhs) {
    return NonDefaultValue(lhs.value && !rhs.value);
  }
};

template <class D>
std::unordered_map<npa::Symbol, npa::DomVal<D>>
toMap(const std::vector<std::pair<npa::Symbol, npa::DomVal<D>>> &pairs) {
  std::unordered_map<npa::Symbol, npa::DomVal<D>> out;
  for (const auto &p : pairs)
    out.emplace(p.first, p.second);
  return out;
}

template <class D>
std::unordered_map<npa::Symbol, npa::DomVal<D>>
evalSystem(const std::vector<std::pair<npa::Symbol, npa::E0<D>>> &eqns,
           const std::unordered_map<npa::Symbol, npa::DomVal<D>> &env) {
  std::unordered_map<npa::Symbol, npa::DomVal<D>> out;
  for (const auto &eqn : eqns)
    out.emplace(eqn.first, npa::I0<D>::eval(false, env, eqn.second));
  return out;
}

} // namespace

TEST(NPA, HoleCanReferenceOtherEquationVariable) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  // x = y
  // y = 1
  //
  // This exercises:
  // - Exp0::Hole lookup against ν in I0 (Kleene/NPA evaluation)
  // - Exp1::Hole lookup against the linear-solver environment in I1 (Newton
  // step)
  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  auto kleeneRes = npa::KleeneSolver<D>::solve(eqns);
  auto kleeneMap = toMap<D>(kleeneRes.first);
  EXPECT_TRUE(kleeneMap.at("y"));
  EXPECT_TRUE(kleeneMap.at("x"));

  auto newtonRes = npa::NPASolver<D>::solve(eqns);
  auto newtonMap = toMap<D>(newtonRes.first);
  EXPECT_TRUE(newtonMap.at("y"));
  EXPECT_TRUE(newtonMap.at("x"));
  EXPECT_TRUE(newtonRes.second.converged);
  EXPECT_FALSE(newtonRes.second.hit_limit);
  EXPECT_FALSE(newtonRes.second.hit_outer_limit);
  EXPECT_FALSE(newtonRes.second.hit_linear_limit);
  EXPECT_FALSE(newtonRes.second.hit_fixpoint_limit);
}

TEST(NPA, SolversRejectDuplicateEquationSymbols) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::term(D::zero()));
  eqns.emplace_back("x", Exp::term(D::one()));

  EXPECT_THROW((void)npa::KleeneSolver<D>::solve(eqns),
               npa::InvalidEquationSystemError);
  EXPECT_THROW((void)npa::NPASolver<D>::solve(eqns),
               npa::InvalidEquationSystemError);

  std::vector<std::pair<npa::Symbol, npa::E1<D>>> linear_eqns;
  linear_eqns.emplace_back("x", npa::Exp1<D>::term(D::zero()));
  linear_eqns.emplace_back("x", npa::Exp1<D>::term(D::one()));
  EXPECT_THROW((void)npa::solve_linear_scc_impl<D>(false, linear_eqns,
                                                   {D::zero(), D::zero()}),
               npa::InvalidEquationSystemError);
}

TEST(NPA, SolversRejectUndefinedEquationSymbols) {
  using D = BoolSemiring;
  using Exp0 = npa::Exp0<D>;
  using Exp1 = npa::Exp1<D>;

  std::vector<std::pair<npa::Symbol, npa::E0<D>>> equations;
  equations.emplace_back("x", Exp0::hole("missing"));
  EXPECT_THROW((void)npa::KleeneSolver<D>::solve(equations),
               npa::InvalidEquationSystemError);
  EXPECT_THROW((void)npa::NPASolver<D>::solve(equations),
               npa::InvalidEquationSystemError);

  std::vector<std::pair<npa::Symbol, npa::E1<D>>> linear_equations;
  linear_equations.emplace_back("x", Exp1::hole("missing"));
  EXPECT_THROW(
      (void)npa::solve_linear_scc_impl<D>(false, linear_equations, {D::zero()}),
      npa::InvalidEquationSystemError);
}

TEST(NPA, SolverRejectsUnboundLocalSymbols) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, npa::E0<D>>> equations;
  equations.emplace_back("x", Exp::bound("local"));
  EXPECT_THROW((void)npa::KleeneSolver<D>::solve(equations),
               npa::InvalidEquationSystemError);
}

TEST(NPA, ProjectDispatchPrefersProjectWhenBothApisExist) {
  using D = BothProjectBoolSemiring;
  EXPECT_TRUE(npa::domain_project<D>(D::one()));
}

TEST(NPA, UnsupportedProjectThrowsInsteadOfReturningZero) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, npa::E0<D>>> equations;
  equations.emplace_back("x", Exp::project(Exp::term(D::one())));
  EXPECT_THROW((void)npa::KleeneSolver<D>::solve(equations),
               npa::UnsupportedDomainProjectError);
}

TEST(NPA, SolversAcceptNonDefaultConstructibleDomainValues) {
  using D = NonDefaultSemiring;
  using Exp = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, npa::E0<D>>> equations;
  equations.emplace_back("x", Exp::term(D::one()));

  auto kleene = npa::KleeneSolver<D>::solve(equations);
  auto newton = npa::NPASolver<D>::solve(equations);
  ASSERT_EQ(kleene.first.size(), 1u);
  ASSERT_EQ(newton.first.size(), 1u);
  EXPECT_TRUE(kleene.first.front().second.value);
  EXPECT_TRUE(newton.first.front().second.value);
}

TEST(NPA, WidthDependentDomainsRejectMissingWidthScope) {
  EXPECT_THROW((void)npa::BitSetDomain::zero(), std::logic_error);
  EXPECT_THROW((void)npa::BitSetDomain::one(), std::logic_error);
}

TEST(NPA, SolverReportsWhenOuterIterationCapReturnsApproximation) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  auto capped = npa::KleeneSolver<D>::solve(eqns, false, 1);
  auto cappedMap = toMap<D>(capped.first);

  EXPECT_FALSE(cappedMap.at("x"));
  EXPECT_TRUE(cappedMap.at("y"));
  EXPECT_FALSE(capped.second.converged);
  EXPECT_TRUE(capped.second.hit_limit);
  EXPECT_TRUE(capped.second.hit_outer_limit);
  EXPECT_FALSE(capped.second.hit_linear_limit);
  EXPECT_FALSE(capped.second.hit_fixpoint_limit);
  EXPECT_EQ(capped.second.equation_count, 2);
  EXPECT_EQ(capped.second.requested_max_iters, 1);
  EXPECT_EQ(capped.second.effective_max_iters, 1);
  EXPECT_EQ(capped.second.linear_strategy, npa::LinearStrategy::SCC);
}

TEST(NPA, ExactModeNewtonReportsNoApproximationSources) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  auto result = npa::NPASolver<D>::solve(eqns);
  auto solved = toMap<D>(result.first);

  EXPECT_TRUE(solved.at("x"));
  EXPECT_TRUE(solved.at("y"));
  EXPECT_TRUE(result.second.converged);
  EXPECT_FALSE(result.second.hit_limit);
  EXPECT_FALSE(result.second.used_approx_equal);
  EXPECT_FALSE(result.second.hit_outer_limit);
  EXPECT_FALSE(result.second.hit_linear_limit);
  EXPECT_FALSE(result.second.hit_fixpoint_limit);
}

TEST(NPA, NewtonInitMatchesFOfBottomAndApproximantsAreMonotone) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  std::unordered_map<npa::Symbol, npa::DomVal<D>> bottom;
  for (const auto &eqn : eqns)
    bottom.emplace(eqn.first, D::zero());
  auto f0 = evalSystem<D>(eqns, bottom);

  auto nu0 = npa::NewtonIter<D>::init(eqns);
  auto nu0Map = toMap<D>(nu0);
  EXPECT_EQ(nu0Map, f0);

  auto nu1 = npa::NewtonIter<D>::run(false, eqns, nu0);
  auto nu1Map = toMap<D>(nu1);
  auto fNu0 = evalSystem<D>(eqns, nu0Map);

  for (const auto &eqn : eqns) {
    const auto &sym = eqn.first;
    EXPECT_TRUE(npa::domain_leq_idempotent<D>(nu0Map.at(sym), fNu0.at(sym)));
    EXPECT_TRUE(npa::domain_leq_idempotent<D>(fNu0.at(sym), nu1Map.at(sym)));
    EXPECT_TRUE(npa::domain_leq_idempotent<D>(nu0Map.at(sym), nu1Map.at(sym)));
  }
}

TEST(NPA, NewtonDominatesKleeneOnIdempotentSystem) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  auto k0 = npa::KleeneIter<D>::init(eqns);
  auto k1 = npa::KleeneIter<D>::run(false, eqns, k0);
  auto nu0 = npa::NewtonIter<D>::init(eqns);
  auto nu1 = npa::NewtonIter<D>::run(false, eqns, nu0);

  auto k0Map = toMap<D>(k0);
  auto k1Map = toMap<D>(k1);
  auto nu0Map = toMap<D>(nu0);
  auto nu1Map = toMap<D>(nu1);

  for (const auto &eqn : eqns) {
    const auto &sym = eqn.first;
    EXPECT_TRUE(npa::domain_leq_idempotent<D>(k0Map.at(sym), nu0Map.at(sym)));
    EXPECT_TRUE(npa::domain_leq_idempotent<D>(k1Map.at(sym), nu1Map.at(sym)));
  }
}

TEST(NPA, NewtonIdempotentUpdateMatchesSolvedLinearizedSystem) {
  using D = BoolSemiring;
  using Exp0 = npa::Exp0<D>;
  using E0 = npa::E0<D>;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("x", Exp0::hole("y"));
  eqns.emplace_back("y", Exp0::term(D::one()));

  auto nu0 = npa::NewtonIter<D>::init(eqns);
  auto nu0Map = toMap<D>(nu0);

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  for (const auto &eqn : eqns) {
    auto fNu = npa::I0<D>::eval(false, nu0Map, eqn.second);
    auto diff = npa::Diff<D>::build(nu0Map, eqn.second);
    rhs.emplace_back(eqn.first, Exp1::add(Exp1::term(fNu), diff));
  }

  std::vector<npa::DomVal<D>> init(rhs.size(), D::zero());
  auto delta = npa::solve_linear_scc_impl<D>(false, rhs, init);
  auto nu1 = npa::NewtonIter<D>::run(false, eqns, nu0);

  for (size_t i = 0; i < nu1.size(); ++i)
    EXPECT_EQ(nu1[i].second, delta[i]);
}

TEST(NPA, SparseNewtonMatchesDenseAndDiscoversRoundSpecificSupport) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  // Paper example:
  //   x1 = 1, x2 = x1, x4 = x1, x3 = x2 * x4.
  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x1", Exp::term(D::one()));
  eqns.emplace_back("x2", Exp::hole("x1"));
  eqns.emplace_back("x3", Exp::mul(Exp::hole("x2"), Exp::hole("x4")));
  eqns.emplace_back("x4", Exp::hole("x1"));

  auto solveWith = [&](npa::NewtonRoundStrategy strategy) {
    npa::SolveOptions options;
    options.newton_round_strategy = strategy;
    options.convergence_policy = npa::ConvergencePolicy::Exact;
    return npa::NPASolver<D>::solve(eqns, options);
  };

  auto dense = solveWith(npa::NewtonRoundStrategy::Dense);
  auto static_slice = solveWith(npa::NewtonRoundStrategy::Static);
  auto always_maybe = solveWith(npa::NewtonRoundStrategy::AlwaysMaybe);
  auto sparse = solveWith(npa::NewtonRoundStrategy::Sparse);

  EXPECT_EQ(toMap<D>(dense.first), toMap<D>(static_slice.first));
  EXPECT_EQ(toMap<D>(dense.first), toMap<D>(always_maybe.first));
  EXPECT_EQ(toMap<D>(dense.first), toMap<D>(sparse.first));
  EXPECT_TRUE(toMap<D>(sparse.first).at("x3"));

  EXPECT_EQ(sparse.second.newton_round_strategy,
            npa::NewtonRoundStrategy::Sparse);
  EXPECT_EQ(sparse.second.indexed_derivative_occurrences, 4);
  ASSERT_GE(sparse.second.newton_rounds.size(), 2u);
  EXPECT_EQ(sparse.second.newton_rounds[0].active_coordinates, 3);
  EXPECT_EQ(sparse.second.newton_rounds[0].queried_occurrences, 4);
  EXPECT_EQ(sparse.second.newton_rounds[0].retained_occurrences, 2);
  EXPECT_EQ(sparse.second.newton_rounds[0].materialized_derivative_terms, 2);
  EXPECT_EQ(sparse.second.newton_rounds[1].active_coordinates, 4);
  EXPECT_EQ(sparse.second.newton_rounds[1].retained_occurrences, 4);

  ASSERT_FALSE(static_slice.second.newton_rounds.empty());
  EXPECT_EQ(static_slice.second.newton_rounds[0].active_coordinates, 4);
  EXPECT_EQ(static_slice.second.newton_rounds[0].queried_occurrences, 0);
  ASSERT_FALSE(always_maybe.second.newton_rounds.empty());
  EXPECT_EQ(always_maybe.second.newton_rounds[0].active_coordinates, 4);
  EXPECT_EQ(always_maybe.second.newton_rounds[0].queried_occurrences, 4);

  auto dense_round = npa::NewtonIter<D>::init(eqns);
  auto sparse_round = dense_round;
  for (int round = 0; round < 3; ++round) {
    dense_round = npa::NewtonIter<D>::run(false, eqns, dense_round);
    sparse_round = npa::NewtonIter<D>::run(false, eqns, sparse_round,
                                           npa::LinearStrategy::SCC,
                                           npa::NewtonRoundStrategy::Sparse);
    EXPECT_EQ(toMap<D>(dense_round), toMap<D>(sparse_round));
  }
}

TEST(NPA, SparseNewtonOneRoundZeroExtendsInactiveCoordinates) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x1", Exp::term(D::one()));
  eqns.emplace_back("x2", Exp::hole("x1"));
  eqns.emplace_back("x3", Exp::mul(Exp::hole("x2"), Exp::hole("x4")));
  eqns.emplace_back("x4", Exp::hole("x1"));

  auto nu0 = npa::NewtonIter<D>::init(eqns);
  auto nu1 = npa::NewtonIter<D>::run(false, eqns, nu0, npa::LinearStrategy::SCC,
                                     npa::NewtonRoundStrategy::Sparse);
  auto values = toMap<D>(nu1);

  EXPECT_TRUE(values.at("x1"));
  EXPECT_TRUE(values.at("x2"));
  EXPECT_FALSE(values.at("x3"));
  EXPECT_TRUE(values.at("x4"));
}

TEST(NPA, SparseFilteredDifferentiationCoversAllNewtonExpressionContexts) {
  using D = SparseProjectedBoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  E star = Exp::star(Exp::ndet(Exp::term(D::one()),
                               Exp::mul(Exp::bound("local"), Exp::hole("f"))),
                     "local");
  E call = Exp::call("f", Exp::hole("seed"));
  E concat = Exp::concat(Exp::hole("seed"), "f", Exp::hole("seed"));
  E branches = Exp::cond(true, call, Exp::hole("star"));

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("seed", Exp::term(D::one()));
  eqns.emplace_back("f", Exp::seq(D::one(), Exp::hole("seed")));
  eqns.emplace_back("star", std::move(star));
  eqns.emplace_back("result", Exp::project(Exp::ndet(std::move(branches),
                                                     std::move(concat))));

  npa::SolveOptions dense_options;
  dense_options.convergence_policy = npa::ConvergencePolicy::Exact;
  auto dense = npa::NPASolver<D>::solve(eqns, dense_options);

  for (npa::NewtonRoundStrategy strategy :
       {npa::NewtonRoundStrategy::Static, npa::NewtonRoundStrategy::AlwaysMaybe,
        npa::NewtonRoundStrategy::Sparse}) {
    npa::SolveOptions options = dense_options;
    options.newton_round_strategy = strategy;
    auto result = npa::NPASolver<D>::solve(eqns, options);
    EXPECT_EQ(toMap<D>(dense.first), toMap<D>(result.first));
    EXPECT_EQ(dense.second.iters, result.second.iters);
  }
}

TEST(NPA, SparseFilteredDifferentiationPreservesNoncommutativeContextOrder) {
  using D = npa::TransformerSummary<char>;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("seed", Exp::term(D::singleton('s')));
  eqns.emplace_back("a", Exp::seq(D::singleton('a'), Exp::hole("seed")));
  eqns.emplace_back("b",
                    Exp::mul(Exp::hole("a"), Exp::term(D::singleton('b'))));
  eqns.emplace_back("c", Exp::call("b", Exp::term(D::singleton('c'))));
  eqns.emplace_back("result", Exp::concat(Exp::term(D::singleton('l')), "c",
                                          Exp::term(D::singleton('r'))));

  npa::SolveOptions dense_options;
  dense_options.convergence_policy = npa::ConvergencePolicy::Exact;
  auto dense = npa::NPASolver<D>::solve(eqns, dense_options);

  for (npa::LinearStrategy backend :
       {npa::LinearStrategy::Naive, npa::LinearStrategy::SCC}) {
    npa::SolveOptions sparse_options = dense_options;
    sparse_options.linear_strategy = backend;
    sparse_options.newton_round_strategy = npa::NewtonRoundStrategy::Sparse;
    auto sparse = npa::NPASolver<D>::solve(eqns, sparse_options);

    EXPECT_EQ(toMap<D>(dense.first), toMap<D>(sparse.first));
    EXPECT_EQ(dense.second.iters, sparse.second.iters);
  }
}

TEST(NPA, SparseDiscoveryRetainsEveryOccurrenceOfTheSameGraphEdge) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("seed", Exp::term(D::one()));
  eqns.emplace_back("x", Exp::hole("seed"));
  eqns.emplace_back("y", Exp::mul(Exp::hole("x"), Exp::hole("x")));

  npa::SolveOptions options;
  options.newton_round_strategy = npa::NewtonRoundStrategy::Sparse;
  options.convergence_policy = npa::ConvergencePolicy::Exact;
  auto result = npa::NPASolver<D>::solve(eqns, options);

  EXPECT_EQ(result.second.indexed_derivative_occurrences, 3);
  ASSERT_GE(result.second.newton_rounds.size(), 2u);
  EXPECT_EQ(result.second.newton_rounds[0].retained_occurrences, 1);
  EXPECT_EQ(result.second.newton_rounds[1].retained_occurrences, 3);
  EXPECT_TRUE(toMap<D>(result.first).at("y"));
}

TEST(NPA, SparseLazyIndexCoalescesSharedDagLeavesWithoutLosingInfluence) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  E shared = Exp::hole("seed");
  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("seed", Exp::term(D::one()));
  eqns.emplace_back(
      "result",
      Exp::ndet(Exp::mul(Exp::term(D::zero()), shared), shared));

  npa::SolveOptions denseOptions;
  denseOptions.convergence_policy = npa::ConvergencePolicy::Exact;
  auto dense = npa::NPASolver<D>::solve(eqns, denseOptions);

  npa::SolveOptions sparseOptions = denseOptions;
  sparseOptions.newton_round_strategy = npa::NewtonRoundStrategy::Sparse;
  auto sparse = npa::NPASolver<D>::solve(eqns, sparseOptions);

  EXPECT_EQ(toMap<D>(dense.first), toMap<D>(sparse.first));
  EXPECT_TRUE(toMap<D>(sparse.first).at("result"));
  EXPECT_EQ(sparse.second.indexed_derivative_occurrences, 1);
  ASSERT_FALSE(sparse.second.newton_rounds.empty());
  EXPECT_EQ(sparse.second.newton_rounds[0].queried_occurrences, 1);
  EXPECT_EQ(sparse.second.newton_rounds[0].retained_occurrences, 1);
}

TEST(NPA, SparseGenKillRepresentationComposesAndAppliesExactly) {
  using D = npa::GenKillTransformer;

  D::value_type inner;
  inner.kill.set(1);
  inner.gen.set(2);
  D::value_type outer;
  outer.kill.set(2);
  outer.gen.set(4097);

  D::fact_type input;
  input.set(0);
  input.set(1);
  D::fact_type sequential = D::apply(outer, D::apply(inner, input));
  D::fact_type composed = D::apply(D::extend(outer, inner), input);
  EXPECT_EQ(composed, sequential);
  EXPECT_TRUE(composed.test(0));
  EXPECT_TRUE(composed.test(4097));
  EXPECT_EQ(composed.count(), 2u);

  const auto constant = D::extend(D::generate(7), D::zero());
  EXPECT_TRUE(constant.kill_all);
  D::fact_type constantResult = D::apply(constant, input);
  EXPECT_TRUE(constantResult.test(7));
  EXPECT_EQ(constantResult.count(), 1u);
  EXPECT_TRUE(D::equal(D::extend(D::zero(), D::generate(7)), D::zero()));
  EXPECT_TRUE(D::equal(D::combine(D::zero(), outer), outer));
}

TEST(NPA, PersistentSparseFactSetMatchesFiniteSetOperations) {
  npa::SparseFactSet lhs;
  npa::SparseFactSet rhs;
  std::set<unsigned> lhsReference;
  std::set<unsigned> rhsReference;
  for (unsigned bit = 0; bit < 4096; ++bit) {
    if ((bit * 17 + 3) % 11 < 4) {
      lhs.set(bit);
      lhsReference.insert(bit);
    }
    if ((bit * 29 + 5) % 13 < 5) {
      rhs.set(bit);
      rhsReference.insert(bit);
    }
  }

  auto materialize = [](const npa::SparseFactSet &facts) {
    return std::set<unsigned>(facts.begin(), facts.end());
  };
  EXPECT_EQ(materialize(lhs), lhsReference);
  EXPECT_EQ(materialize(rhs), rhsReference);

  npa::SparseFactSet joined = lhs;
  joined |= rhs;
  std::set<unsigned> joinedReference = lhsReference;
  joinedReference.insert(rhsReference.begin(), rhsReference.end());
  EXPECT_EQ(materialize(joined), joinedReference);

  npa::SparseFactSet common = lhs;
  common &= rhs;
  std::set<unsigned> commonReference;
  std::set_intersection(lhsReference.begin(), lhsReference.end(),
                        rhsReference.begin(), rhsReference.end(),
                        std::inserter(commonReference, commonReference.end()));
  EXPECT_EQ(materialize(common), commonReference);

  npa::SparseFactSet difference = lhs;
  difference.intersectWithComplement(rhs);
  std::set<unsigned> differenceReference;
  std::set_difference(lhsReference.begin(), lhsReference.end(),
                      rhsReference.begin(), rhsReference.end(),
                      std::inserter(differenceReference,
                                    differenceReference.end()));
  EXPECT_EQ(materialize(difference), differenceReference);

  npa::SparseFactSet unchanged = lhs;
  const unsigned existing = *lhsReference.begin();
  EXPECT_FALSE(unchanged.set(existing));
  EXPECT_FALSE(unchanged.reset(4097));
  EXPECT_EQ(unchanged, lhs);
}

TEST(NPA, SparseTaintRelationMatchesSequentialAndClosureSemantics) {
  using D = npa::TaintTransformer;

  auto inner = D::one();
  D::clearOutput(inner, 2);
  D::addEdge(inner, 0, 2);
  D::addGen(inner, 3);

  auto outer = D::one();
  D::kill(outer, 1);
  D::addEdge(outer, 2, 4);
  D::addGen(outer, 5);

  D::fact_type input;
  input.set(0);
  input.set(1);
  input.set(2);
  auto sequential = D::apply(outer, D::apply(inner, input));
  auto composed = D::apply(D::extend(outer, inner), input);
  EXPECT_EQ(composed, sequential);

  auto joinedResult = D::apply(D::combine(outer, inner), input);
  auto expectedJoin = D::apply(outer, input);
  expectedJoin |= D::apply(inner, input);
  EXPECT_EQ(joinedResult, expectedJoin);

  auto step = D::zero();
  D::addEdge(step, 0, 1);
  D::addEdge(step, 1, 2);
  D::fact_type seed;
  seed.set(0);
  auto closed = D::apply(D::star(step), seed);
  EXPECT_TRUE(closed.test(0));
  EXPECT_TRUE(closed.test(1));
  EXPECT_TRUE(closed.test(2));
  EXPECT_EQ(closed.count(), 3u);
}

TEST(NPA, SparseOracleHonorsDirectionalZeroMapContracts) {
  using D = npa::GenKillTransformer;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  D::value_type generating = D::generate(0);

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("seed", Exp::term(generating));
  // For this transformer carrier, a ⊗ zero may still generate facts, while
  // zero ⊗ a is always zero.
  eqns.emplace_back("right_zero",
                    Exp::mul(Exp::hole("seed"), Exp::term(D::zero())));
  eqns.emplace_back("left_zero",
                    Exp::mul(Exp::term(D::zero()), Exp::hole("seed")));

  npa::SolveOptions dense_options;
  dense_options.convergence_policy = npa::ConvergencePolicy::Exact;
  auto dense = npa::NPASolver<D>::solve(eqns, dense_options);

  npa::SolveOptions sparse_options = dense_options;
  sparse_options.newton_round_strategy = npa::NewtonRoundStrategy::Sparse;
  auto sparse = npa::NPASolver<D>::solve(eqns, sparse_options);

  EXPECT_EQ(toMap<D>(dense.first), toMap<D>(sparse.first));
  EXPECT_FALSE(D::equal(toMap<D>(sparse.first).at("right_zero"), D::zero()));
  EXPECT_TRUE(D::equal(toMap<D>(sparse.first).at("left_zero"), D::zero()));
  ASSERT_FALSE(sparse.second.newton_rounds.empty());
  EXPECT_EQ(sparse.second.newton_rounds[0].active_coordinates, 2);
  EXPECT_EQ(sparse.second.newton_rounds[0].queried_occurrences, 2);
  EXPECT_EQ(sparse.second.newton_rounds[0].retained_occurrences, 1);
}

TEST(NPA, SparseOracleWithoutDomainLawsConservativelyReturnsMaybe) {
  using D = NonDefaultSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("seed", Exp::term(D::one()));
  eqns.emplace_back("left", Exp::hole("seed"));
  eqns.emplace_back("right", Exp::hole("seed"));
  eqns.emplace_back("product", Exp::mul(Exp::hole("left"), Exp::hole("right")));

  npa::SolveOptions options;
  options.newton_round_strategy = npa::NewtonRoundStrategy::Sparse;
  options.convergence_policy = npa::ConvergencePolicy::Exact;
  auto result = npa::NPASolver<D>::solve(eqns, options);

  ASSERT_FALSE(result.second.newton_rounds.empty());
  EXPECT_EQ(result.second.newton_rounds[0].active_coordinates, 4);
  EXPECT_EQ(result.second.newton_rounds[0].retained_occurrences, 4);
  EXPECT_TRUE(toMap<D>(result.first).at("product").value);
}

namespace {

struct TraceSemiring {
  using value_type = std::string;
  using test_type = bool;
  static constexpr bool idempotent = false;

  static value_type zero() { return "0"; }
  static value_type one() { return "1"; }

  static bool equal(const value_type &a, const value_type &b) { return a == b; }
  static value_type combine(const value_type &a, const value_type &b) {
    return "(" + a + "+" + b + ")";
  }
  static value_type extend(const value_type &a, const value_type &b) {
    return "(" + a + "*" + b + ")";
  }
  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }
  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }
  static value_type subtract(const value_type &a, const value_type &b) {
    return "(" + a + "-" + b + ")";
  }
};

struct BadDeltaSemiring {
  using value_type = int;
  using test_type = bool;
  static constexpr bool idempotent = false;

  static value_type zero() { return 0; }
  static value_type one() { return 1; }

  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a + b; }
  static value_type extend(value_type a, value_type b) { return a * b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }

  // Intentionally invalid: combine(nu, subtract(f(nu), nu)) != f(nu) in
  // general.
  static value_type subtract(value_type, value_type) { return 0; }
};

struct ExactDeltaSemiring {
  using value_type = int;
  using test_type = bool;
  static constexpr bool idempotent = false;

  static value_type zero() { return 0; }
  static value_type one() { return 1; }
  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a + b; }
  static value_type extend(value_type a, value_type b) { return a * b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a - b; }
};

struct ApproxEqualityBoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = true;

  static void resetApproxCounter() { ApproxCounter = 0; }
  static int getApproxCounter() { return ApproxCounter; }

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type a, value_type b) { return a == b; }
  static bool approx_equal(value_type a, value_type b) {
    return a == b && ApproxCounter++ >= 3;
  }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }

private:
  static int ApproxCounter;
};

int ApproxEqualityBoolSemiring::ApproxCounter = 0;

struct LimitedLinearBoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr long max_linear_steps = 0;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
};

struct LimitedFixpointBoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr int max_fixpoint_iters = 0;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
};

struct ContractViolationSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type, value_type) { return false; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
};

struct SampledLawViolationSemiring {
  using value_type = int;
  using test_type = bool;
  static constexpr bool idempotent = true;

  static value_type zero() { return 0; }
  static value_type one() { return 1; }
  static bool equal(value_type lhs, value_type rhs) { return lhs == rhs; }
  static value_type combine(value_type lhs, value_type rhs) {
    return std::max(lhs, rhs);
  }
  static value_type extend(value_type lhs, value_type rhs) {
    if (lhs == 0 || rhs == 0)
      return 0;
    if (lhs == 1)
      return rhs;
    if (rhs == 1)
      return lhs;
    return 0;
  }
  static value_type extend_lin(value_type lhs, value_type rhs) {
    return extend(lhs, rhs);
  }
  static value_type ndetCombine(value_type lhs, value_type rhs) {
    return combine(lhs, rhs);
  }
  static value_type condCombine(test_type condition, value_type then_value,
                                value_type else_value) {
    return condition ? then_value : else_value;
  }
  static value_type subtract(value_type lhs, value_type rhs) {
    return lhs > rhs ? lhs : 0;
  }
};

struct UnsafeProjectedBoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = true;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
  static value_type project(value_type v) { return v; }
};

struct WriteOp {
  const void *dest = nullptr;

  bool operator<(const WriteOp &other) const { return dest < other.dest; }
  bool operator==(const WriteOp &other) const { return dest == other.dest; }
};

} // namespace

namespace npa {
template <> struct TensorSemiringTraits<BadDeltaSemiring> {
  using tensor_domain = TensorProductLift<BadDeltaSemiring>;

  static bool available() { return false; }
  static bool paper_admissible() { return false; }

  static tensor_domain::value_type
  right_constant(const BadDeltaSemiring::value_type &v) {
    return {BadDeltaSemiring::one(), v};
  }

  static tensor_domain::value_type
  left_constant(const BadDeltaSemiring::value_type &v) {
    return {v, BadDeltaSemiring::one()};
  }

  static tensor_domain::value_type
  couple(const BadDeltaSemiring::value_type &lhs,
         const BadDeltaSemiring::value_type &rhs) {
    return {lhs, rhs};
  }

  static BadDeltaSemiring::value_type
  readout(const tensor_domain::value_type &v) {
    return tensor_domain::project(v);
  }
};
} // namespace npa

namespace {

TEST(NPA, ConcatRepresentsTwoSidedMultiplication) {
  using D = TraceSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["x"] = "X";

  // A · x · B
  E e = Exp::concat(Exp::term("A"), "x", Exp::term("B"));
  auto v = npa::I0<D>::eval(false, nu, e);
  EXPECT_EQ(v, "(A*(X*B))");
}

TEST(NPA, BoundVariablesDoNotAliasEquationVariables) {
  using D = TraceSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["x"] = "GLOBAL";

  // mu(body, x) where body references x as a *bound* variable.
  // With our TraceSemiring, fixpoint starting at 0:
  //   cur0=0, body = (A*cur), so it stabilizes at "0" only if A is "1".
  // Use body = bound(x) so result should be the initial "0".
  E e = Exp::mu(Exp::bound("x"), "x");
  auto v = npa::I0<D>::eval(false, nu, e);
  EXPECT_EQ(v, "0");
}

TEST(NPA, InvalidNonIdempotentDeltaFailsFast) {
  using D = BadDeltaSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::ndet(Exp::hole("x"), Exp::term(1)));

  EXPECT_THROW((void)npa::NPASolver<D>::solve(eqns),
               npa::InvalidNewtonDeltaError);
}

TEST(NPA, ValidNonIdempotentResidualReconstructsFunctionValue) {
  using D = ExactDeltaSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["x"] = 2;

  E expr = Exp::ndet(Exp::hole("x"), Exp::term(3));
  auto fNu = npa::I0<D>::eval(false, nu, expr);
  auto delta = D::subtract(fNu, nu.at("x"));

  EXPECT_EQ(fNu, 5);
  EXPECT_EQ(delta, 3);
  EXPECT_NO_THROW(npa::require_valid_newton_delta<D>(fNu, nu.at("x"), delta));
}

TEST(NPA, SparseRoundStrategiesRejectNonIdempotentDomains) {
  using D = BadDeltaSemiring;
  using Exp = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, npa::E0<D>>> eqns;
  eqns.emplace_back("x", Exp::term(D::one()));

  for (npa::NewtonRoundStrategy strategy :
       {npa::NewtonRoundStrategy::Static, npa::NewtonRoundStrategy::AlwaysMaybe,
        npa::NewtonRoundStrategy::Sparse}) {
    npa::SolveOptions options;
    options.max_iterations = 1;
    options.newton_round_strategy = strategy;
    EXPECT_THROW((void)npa::NPASolver<D>::solve(eqns, options),
                 npa::SparseNewtonRequiresIdempotentError);
  }
}

TEST(NPA, AutomaticNIterationBoundFallsBackWhenEqualityIsApproximate) {
  using D = ApproxEqualityBoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  D::resetApproxCounter();

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::term(D::one()));

  testing::internal::CaptureStderr();
  auto result = npa::NPASolver<D>::solve(eqns, true);
  std::string stderrOutput = testing::internal::GetCapturedStderr();

  auto solved = toMap<D>(result.first);
  EXPECT_FALSE(result.second.converged);
  EXPECT_FALSE(result.second.hit_limit);
  EXPECT_TRUE(result.second.used_approx_equal);
  EXPECT_TRUE(result.second.used_auto_n_cap);
  EXPECT_TRUE(result.second.retried_without_auto_n_cap);
  EXPECT_FALSE(result.second.hit_outer_limit);
  EXPECT_FALSE(result.second.hit_linear_limit);
  EXPECT_FALSE(result.second.hit_fixpoint_limit);
  EXPECT_TRUE(solved.at("x"));
  EXPECT_GE(D::getApproxCounter(), 4);
  EXPECT_NE(stderrOutput.find("automatic n-iteration bound was insufficient"),
            std::string::npos);
}

TEST(NPA, ExactConvergencePolicyOverridesDomainApproximateEquality) {
  using D = ApproxEqualityBoolSemiring;
  using Exp = npa::Exp0<D>;

  D::resetApproxCounter();
  std::vector<std::pair<npa::Symbol, npa::E0<D>>> eqns;
  eqns.emplace_back("x", Exp::term(D::one()));

  auto result = npa::NPASolver<D>::solve(
      eqns, false, -1, npa::LinearStrategy::SCC, npa::DomainContractMode::Off,
      npa::ConvergencePolicy::Exact);

  EXPECT_TRUE(result.second.converged);
  EXPECT_FALSE(result.second.used_approx_equal);
  EXPECT_EQ(result.second.convergence_policy, npa::ConvergencePolicy::Exact);
  EXPECT_EQ(D::getApproxCounter(), 0);
}

TEST(NPA, SolverCanReportDomainContractCheckFailures) {
  using D = ContractViolationSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::term(D::one()));

  auto result =
      npa::NPASolver<D>::solve(eqns, false, 1, npa::LinearStrategy::SCC,
                               npa::DomainContractMode::BasicChecks);

  EXPECT_TRUE(result.second.domain_contract_checks_run);
  EXPECT_TRUE(result.second.domain_contract_checks_failed);
}

TEST(NPA, StrictDomainContractModeRejectsInvalidDomain) {
  using D = ContractViolationSemiring;
  using Exp = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, npa::E0<D>>> eqns;
  eqns.emplace_back("x", Exp::term(D::one()));

  EXPECT_THROW((void)npa::NPASolver<D>::solve(eqns, false, 1,
                                              npa::LinearStrategy::SCC,
                                              npa::DomainContractMode::Strict),
               npa::DomainContractViolationError);
  EXPECT_THROW((void)npa::KleeneSolver<D>::solve(
                   eqns, false, 1, npa::DomainContractMode::Strict),
               npa::DomainContractViolationError);
}

TEST(NPA, SampledDomainChecksExerciseRepresentativeValues) {
  using D = SampledLawViolationSemiring;
  EXPECT_TRUE(npa::run_basic_domain_contract_checks<D>());
  EXPECT_FALSE(npa::run_sampled_domain_contract_checks<D>({2}));
}

TEST(NPA, LinearStepLimitMarksNewtonResultAsApproximate) {
  using D = LimitedLinearBoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  auto result =
      npa::NPASolver<D>::solve(eqns, false, 1, npa::LinearStrategy::SCC);

  EXPECT_FALSE(result.second.converged);
  EXPECT_TRUE(result.second.hit_limit);
  EXPECT_TRUE(result.second.hit_outer_limit);
  EXPECT_TRUE(result.second.hit_linear_limit);
  EXPECT_FALSE(result.second.hit_fixpoint_limit);
}

TEST(NPA, NewtonRejectsMuExpressions) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::mu(Exp::term(D::one()), "b"));

  EXPECT_THROW((void)npa::NPASolver<D>::solve(eqns),
               npa::UnsupportedNewtonMuError);
}

TEST(NPA, NewtonRejectsUnsafeProjectExpressions) {
  using D = UnsafeProjectedBoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::project(Exp::term(D::one())));

  EXPECT_THROW((void)npa::NPASolver<D>::solve(eqns),
               npa::UnsafeNewtonProjectError);
}

TEST(NPA, FixpointIterationLimitMarksMuClosureAsApproximate) {
  using D = LimitedFixpointBoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back(
      "x", Exp::mu(Exp::ndet(Exp::bound("b"), Exp::term(D::one())), "b"));

  auto result = npa::KleeneSolver<D>::solve(eqns, false, 2);
  auto solved = toMap<D>(result.first);

  EXPECT_FALSE(solved.at("x"));
  EXPECT_FALSE(result.second.converged);
  EXPECT_TRUE(result.second.hit_limit);
  EXPECT_FALSE(result.second.hit_outer_limit);
  EXPECT_FALSE(result.second.hit_linear_limit);
  EXPECT_TRUE(result.second.hit_fixpoint_limit);
}

TEST(NPA, ZeroFixpointIterationLimitSkipsVectorUpdate) {
  using D = LimitedFixpointBoolSemiring;
  bool invoked = false;
  npa::npa_reset_limit_hit();

  auto result = npa::fix_vec<D>(false, std::vector<bool>{false},
                                [&](const std::vector<bool> &) {
                                  invoked = true;
                                  return std::vector<bool>{true};
                                });

  EXPECT_FALSE(invoked);
  EXPECT_FALSE(result.front());
  EXPECT_TRUE(npa::npa_hit_fixpoint_limit());
}

TEST(NPA, TransformerSummaryPreservesMayWriteAcrossCombineAndExtend) {
  using D = npa::TransformerSummary<WriteOp>;

  static int slot_a = 0;
  static int slot_b = 0;

  auto a = D::singleton(WriteOp{&slot_a});
  auto b = D::singleton(WriteOp{&slot_b});

  auto joined = D::combine(a, b);
  EXPECT_TRUE(joined.mayWrite(&slot_a));
  EXPECT_TRUE(joined.mayWrite(&slot_b));

  auto composed = D::extend(a, b);
  EXPECT_TRUE(composed.mayWrite(&slot_a));
  EXPECT_TRUE(composed.mayWrite(&slot_b));
}

TEST(NPA, TransformerSummaryCondCombineRespectsBooleanGuard) {
  using D = npa::TransformerSummary<char>;

  auto thenV = D::singleton('t');
  auto elseV = D::singleton('e');

  auto chosenThen = D::condCombine(true, thenV, elseV);
  auto chosenElse = D::condCombine(false, thenV, elseV);

  EXPECT_EQ(chosenThen.transformers().size(), 1u);
  EXPECT_TRUE(chosenThen.transformers().count(std::vector<char>{'t'}));
  EXPECT_FALSE(chosenThen.transformers().count(std::vector<char>{'e'}));

  EXPECT_EQ(chosenElse.transformers().size(), 1u);
  EXPECT_TRUE(chosenElse.transformers().count(std::vector<char>{'e'}));
  EXPECT_FALSE(chosenElse.transformers().count(std::vector<char>{'t'}));
}

} // namespace

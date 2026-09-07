#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"
#include "Dataflow/NPA/NPA.h"

#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct BoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = true;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type lhs, value_type rhs) { return lhs == rhs; }
  static value_type combine(value_type lhs, value_type rhs) {
    return lhs || rhs;
  }
  static value_type extend(value_type lhs, value_type rhs) {
    return lhs && rhs;
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
    return lhs && !rhs;
  }
};

template <class D>
std::unordered_map<npa::Symbol, npa::DomVal<D>>
toMap(const std::vector<std::pair<npa::Symbol, npa::DomVal<D>>> &pairs) {
  std::unordered_map<npa::Symbol, npa::DomVal<D>> out;
  for (const auto &pair : pairs)
    out.emplace(pair.first, pair.second);
  return out;
}

} // namespace

TEST(NPAAdaptiveScc, PlanChoosesDirectForAcyclicSingletons) {
  using D = BoolSemiring;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  rhs.emplace_back("A", Exp1::term(true));
  rhs.emplace_back("B", Exp1::hole("A"));

  std::vector<std::pair<npa::Symbol, npa::E1<TD>>> rhs_tensor;
  auto plan = npa::detail::build_linear_scc_plan<D>(rhs);
  npa::detail::NewtonRoundSetup<D> setup;
  auto execution =
      npa::detail::choose_adaptive_scc_backends<D>(plan, rhs_tensor, setup);

  ASSERT_EQ(plan.infos.size(), 2u);
  ASSERT_EQ(execution.sccs.size(), plan.infos.size());
  for (std::size_t sid = 0; sid < plan.infos.size(); ++sid) {
    EXPECT_EQ(execution.sccs[sid].backend, npa::detail::SccBackend::Direct);
    EXPECT_FALSE(plan.infos[sid].is_cyclic);
    EXPECT_FALSE(execution.sccs[sid].tensor_fallback);
  }
}

TEST(NPAAdaptiveScc, PlanChoosesWorklistForNonLcflCycle) {
  using D = BoolSemiring;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  rhs.emplace_back("X", Exp1::hole("Y"));
  rhs.emplace_back("Y", Exp1::add(Exp1::term(true), Exp1::hole("X")));

  std::vector<std::pair<npa::Symbol, npa::E1<TD>>> rhs_tensor;
  auto plan = npa::detail::build_linear_scc_plan<D>(rhs);
  npa::detail::NewtonRoundSetup<D> setup;
  auto execution =
      npa::detail::choose_adaptive_scc_backends<D>(plan, rhs_tensor, setup);

  ASSERT_EQ(plan.infos.size(), 1u);
  ASSERT_EQ(execution.sccs.size(), 1u);
  EXPECT_EQ(execution.sccs.front().backend, npa::detail::SccBackend::Worklist);
  EXPECT_TRUE(plan.infos.front().is_cyclic);
  EXPECT_FALSE(plan.infos.front().has_lcfl_structure);
  EXPECT_FALSE(execution.sccs.front().tensor_fallback);
}

TEST(NPAAdaptiveScc, PlanChoosesTensorForLcflCycle) {
  using D = npa::PredicateRelationDomain;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  D::configure(2, 1);
  E0 set_global_true = Exp0::term(D::assignConst(0, true));
  E0 set_local_true = Exp0::term(D::assignConst(1, true));
  E0 id = Exp0::term(D::one());
  E0 rhs = Exp0::project(
      Exp0::ndet(id, Exp0::concat(set_global_true, "X", set_local_true)));

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("X", rhs);
  auto binds = npa::detail::build_newton_initial_values<D>(eqns);
  auto setup = npa::detail::build_newton_round_setup<D>(
      eqns, binds, npa::LinearStrategy::AdaptiveScc);
  auto plan = npa::detail::build_linear_scc_plan<D>(setup.rhs);
  auto execution = npa::detail::choose_adaptive_scc_backends<D>(
      plan, setup.rhs_tensor, setup);

  ASSERT_EQ(plan.infos.size(), 1u);
  ASSERT_EQ(execution.sccs.size(), 1u);
  EXPECT_EQ(execution.sccs.front().backend, npa::detail::SccBackend::Tensor);
  EXPECT_TRUE(plan.infos.front().is_cyclic);
  EXPECT_TRUE(plan.infos.front().has_lcfl_structure);
  EXPECT_TRUE(execution.sccs.front().tensor_eligible);
}

TEST(NPAAdaptiveScc, PlanTracksTensorFallbackWhenUnavailable) {
  using D = BoolSemiring;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  rhs.emplace_back(
      "X", Exp1::add(Exp1::term(true),
                     Exp1::concat(Exp1::term(true), "X", Exp1::term(true))));

  std::vector<std::pair<npa::Symbol, npa::E1<TD>>> rhs_tensor;
  auto plan = npa::detail::build_linear_scc_plan<D>(rhs);
  npa::detail::NewtonRoundSetup<D> setup;
  auto execution =
      npa::detail::choose_adaptive_scc_backends<D>(plan, rhs_tensor, setup);

  ASSERT_EQ(plan.infos.size(), 1u);
  ASSERT_EQ(execution.sccs.size(), 1u);
  EXPECT_EQ(execution.sccs.front().backend, npa::detail::SccBackend::Worklist);
  EXPECT_TRUE(plan.infos.front().has_lcfl_structure);
  EXPECT_TRUE(execution.sccs.front().tensor_fallback);
  EXPECT_EQ(execution.sccs.front().tensor_fallback_reason,
            npa::detail::TensorFallbackReason::TensorUnavailable);
}

TEST(NPAAdaptiveScc, MatchesSccOnMixedOrdinarySystemAndReportsCounts) {
  using D = BoolSemiring;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("A", Exp0::term(true));
  eqns.emplace_back("X", Exp0::hole("Y"));
  eqns.emplace_back("Y", Exp0::ndet(Exp0::term(true), Exp0::hole("X")));

  auto scc =
      npa::NPASolver<D>::solve(eqns, false, -1, npa::LinearStrategy::SCC);
  auto adaptive = npa::NPASolver<D>::solve(eqns, false, -1,
                                           npa::LinearStrategy::AdaptiveScc);

  EXPECT_EQ(toMap<D>(scc.first), toMap<D>(adaptive.first));
  EXPECT_TRUE(adaptive.second.converged);
  EXPECT_TRUE(adaptive.second.adaptive_scc_used);
  EXPECT_GE(adaptive.second.adaptive_scc_direct_count, 1);
  EXPECT_GE(adaptive.second.adaptive_scc_worklist_count, 1);
  EXPECT_EQ(adaptive.second.adaptive_scc_tensor_count, 0);
  EXPECT_EQ(adaptive.second.adaptive_scc_tensor_fallback_count, 0);
}

TEST(NPAAdaptiveScc, MatchesTensorOnEligibleSystemAndReportsCounts) {
  using D = npa::PredicateRelationDomain;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  D::configure(2, 1);
  E0 set_global_true = Exp0::term(D::assignConst(0, true));
  E0 set_local_true = Exp0::term(D::assignConst(1, true));
  E0 id = Exp0::term(D::one());
  E0 rhs = Exp0::project(
      Exp0::ndet(id, Exp0::concat(set_global_true, "X", set_local_true)));

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("X", rhs);

  auto tensor = npa::NPASolver<D>::solve(eqns, false, -1,
                                         npa::LinearStrategy::TensorProduct);
  auto adaptive = npa::NPASolver<D>::solve(eqns, false, -1,
                                           npa::LinearStrategy::AdaptiveScc);

  ASSERT_EQ(tensor.first.size(), 1u);
  ASSERT_EQ(adaptive.first.size(), 1u);
  EXPECT_TRUE(D::equal(tensor.first[0].second, adaptive.first[0].second));
  EXPECT_TRUE(adaptive.second.adaptive_scc_used);
  EXPECT_EQ(adaptive.second.adaptive_scc_direct_count, 0);
  EXPECT_EQ(adaptive.second.adaptive_scc_worklist_count, 0);
  EXPECT_GE(adaptive.second.adaptive_scc_tensor_count, 1);
  EXPECT_EQ(adaptive.second.adaptive_scc_tensor_fallback_count, 0);
}

TEST(NPAAdaptiveScc, MatchesSccOnMixedTensorAndDirectSystem) {
  using D = npa::PredicateRelationDomain;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  D::configure(2, 1);
  E0 set_global_true = Exp0::term(D::assignConst(0, true));
  E0 set_local_true = Exp0::term(D::assignConst(1, true));
  E0 id = Exp0::term(D::one());
  E0 tensor_rhs = Exp0::project(
      Exp0::ndet(id, Exp0::concat(set_global_true, "X", set_local_true)));

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("A", id);
  eqns.emplace_back("X", tensor_rhs);

  auto scc =
      npa::NPASolver<D>::solve(eqns, false, -1, npa::LinearStrategy::SCC);
  auto adaptive = npa::NPASolver<D>::solve(eqns, false, -1,
                                           npa::LinearStrategy::AdaptiveScc);

  ASSERT_EQ(scc.first.size(), adaptive.first.size());
  for (std::size_t i = 0; i < scc.first.size(); ++i)
    EXPECT_TRUE(D::equal(scc.first[i].second, adaptive.first[i].second));
  EXPECT_GE(adaptive.second.adaptive_scc_direct_count, 1);
  EXPECT_GE(adaptive.second.adaptive_scc_tensor_count, 1);
}

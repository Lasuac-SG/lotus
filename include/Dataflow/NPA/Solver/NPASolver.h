#ifndef NPA_NPA_SOLVER_H
#define NPA_NPA_SOLVER_H

/**
 * \file
 * \brief Public NPA solver for whole equation systems `X = f(X)`.
 *
 * `NPASolver<D>` implements the JACM-style Newton outer algorithm:
 * - ν^(0) = f(⊥)
 * - ν^(i+1) = ν^(i) ⊔ LinearCorrectionTerm
 *
 * The correction term Δ^(i) is the least solution of the linearized system
 * `Df|ν^(i)(X) + δ^(i) = X` (Eqn. (2), (13)); then
 * `ν^(i+1) = ν^(i) ⊕ Δ^(i)` (idempotent) or
 * `ν^(i+1) = ν^(i) + Δ^(i)` (non-idempotent).
 *
 * The inner linearized system is solved by `Naive`, `SCC`, `AdaptiveScc`, or
 * `TensorProduct` (`LinearStrategy`). `AdaptiveScc` keeps NPA's outer
 * iteration unchanged, but chooses the inner linear solver independently for
 * each linearized SCC: direct for singleton acyclic SCCs, SCC worklist for
 * ordinary recursive SCCs, and TOPLAS tensor solving for tensor-eligible
 * cyclic LCFL SCCs.
 *
 * Exact-vs-approximate status:
 * - `Stat::converged` means theorem-faithful convergence: equality stabilized
 *   and no approximation hook fired.
 * - `used_approx_equal`, `hit_outer_limit`, `hit_linear_limit`, and
 *   `hit_fixpoint_limit` record the approximation sources explicitly.
 * - `hit_limit` remains the aggregate of the bounding hooks only.
 * - `adaptive_scc_*` counters aggregate the SCC-local solver choices made
 *   across all adaptive linear solves in the Newton run, not just the final
 *   converged round.
 *
 * `LinearStrategy` selects only the backend used for this inner linearized
 * system. It does not affect `KleeneSolver<D>`, which is a separate public
 * solver in `KleeneSolver.h`.
 *
 * References:
 * - Esparza et al. (JACM): NPA outer algorithm and linearization.
 * - Reps et al. (TOPLAS 2016): optional tensor regularization for suitable
 *   LCFL inner sub-problems.
 */

#include "Dataflow/NPA/Solver/DomainValidation.h"
#include "Dataflow/NPA/Solver/EquationSystem.h"
#include "Dataflow/NPA/Solver/Newton/Linear/AdaptivePlan.h"
#include "Dataflow/NPA/Solver/Newton/Linear/Tensor/TensorSolver.h"
#include "Dataflow/NPA/Solver/SolveContext.h"
#include "Dataflow/NPA/Solver/Statistics.h"

namespace npa {

namespace detail {
/// C++14-friendly dispatch for delta: avoid if constexpr (DomainHasChooseDelta,
/// idempotent) → choose_delta(v, nu) or v; else subtract(v, nu) or v.
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::true_type /* has_choose_delta */,
                        std::true_type /* idempotent */) {
  (void)nu_sym;
  return v;
}
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::true_type /* has_choose_delta */,
                        std::false_type /* idempotent */) {
  return D::choose_delta(v, nu_sym);
}
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::false_type /* has_choose_delta */,
                        std::true_type /* idempotent */) {
  (void)nu_sym;
  return v;
}
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::false_type /* has_choose_delta */,
                        std::false_type /* idempotent */) {
  return D::subtract(v, nu_sym);
}

template <class D> inline void require_newton_compatible_expr(const E0<D> &e) {
  if (ExprFeatureDetector<D>::has_mu(e))
    throw UnsupportedNewtonMuError{};
  if (ExprFeatureDetector<D>::has_project(e) &&
      !domain_project_newton_safe<D>())
    throw UnsafeNewtonProjectError{};
}

template <class D> struct NewtonRoundSetup {
  using tensor_domain = typename TensorSemiringTraits<D>::tensor_domain;
  std::vector<std::pair<Symbol, E1<D>>> rhs;
  std::vector<std::pair<Symbol, E1<tensor_domain>>> rhs_tensor;
  bool has_lcfl_structure = false;
  bool tensor_requested = false;
  bool tensor_available = false;
  bool tensor_admissible = false;
  bool tensor_laws_validated = false;
};

template <class D>
std::vector<std::pair<Symbol, DomVal<D>>>
build_newton_initial_values(const std::vector<std::pair<Symbol, E0<D>>> &eqns) {
  using V = DomVal<D>;
  std::unordered_map<Symbol, V> nu0;
  for (auto &e : eqns)
    nu0.insert_or_assign(e.first, D::zero());

  std::vector<std::pair<Symbol, V>> cur;
  cur.reserve(eqns.size());
  for (auto &e : eqns) {
    require_newton_compatible_expr<D>(e.second);
    cur.emplace_back(e.first, I0<D>::eval(false, nu0, e.second));
  }
  return cur;
}

template <class D>
NewtonRoundSetup<D>
build_newton_round_setup(const std::vector<std::pair<Symbol, E0<D>>> &eqns,
                         const std::vector<std::pair<Symbol, DomVal<D>>> &binds,
                         LinearStrategy linStrat = LinearStrategy::SCC) {
  using V = DomVal<D>;
  using TensorTraits = TensorSemiringTraits<D>;
  using TD = typename TensorTraits::tensor_domain;

  std::unordered_map<Symbol, V> nu;
  for (auto &b : binds)
    nu.insert_or_assign(b.first, b.second);

  NewtonRoundSetup<D> setup;
  setup.tensor_requested = linStrat == LinearStrategy::TensorProduct ||
                           linStrat == LinearStrategy::AdaptiveScc;
  setup.tensor_available = setup.tensor_requested && TensorTraits::available();
  setup.tensor_admissible =
      setup.tensor_available && TensorTraits::paper_admissible();
  setup.tensor_laws_validated =
      setup.tensor_admissible && tensor_paper_laws_validated<D>();

  setup.rhs.reserve(eqns.size());
  if (setup.tensor_laws_validated)
    setup.rhs_tensor.reserve(eqns.size());

  for (const auto &eqn : eqns) {
    require_newton_compatible_expr<D>(eqn.second);
    typename I0<D>::EvaluationContext evaluation_context;
    V v = I0<D>::evalWithContext(nu, {}, eqn.second, evaluation_context);
    V delta0 = compute_delta<D>(
        v, nu.at(eqn.first),
        std::integral_constant<bool, DomainHasChooseDelta<D>::value>{},
        std::integral_constant<bool, D::idempotent>{});
    if (!D::idempotent)
      require_valid_newton_delta<D>(v, nu.at(eqn.first), delta0);
    auto d = Diff<D>::build(nu, eqn.second, evaluation_context);
    setup.has_lcfl_structure =
        setup.has_lcfl_structure || LCFLDetector<D>::has_lcfl_structure(d);
    setup.rhs.emplace_back(eqn.first, Exp1<D>::add(Exp1<D>::term(delta0), d));
    if (setup.tensor_laws_validated) {
      auto tensor_d = TensorDiff<D>::build(nu, eqn.second, evaluation_context);
      E1<TD> tensor_rhs = Exp1<TD>::add(
          Exp1<TD>::term(TensorTraits::right_constant(delta0)), tensor_d);
      if (tensor_supports_projection_equations<D>() && eqn.second &&
          eqn.second->k == Exp0<D>::Project && tensor_d &&
          tensor_d->k == Exp1<TD>::Project) {
        tensor_rhs = Exp1<TD>::project(Exp1<TD>::add(
            Exp1<TD>::term(TensorTraits::right_constant(delta0)), tensor_d->t));
      }
      setup.rhs_tensor.emplace_back(eqn.first, std::move(tensor_rhs));
    }
  }
  return setup;
}

template <class D>
bool tensor_expr_is_projection_sensitive(
    const E1<typename TensorSemiringTraits<D>::tensor_domain> &expr) {
  using TD = typename TensorSemiringTraits<D>::tensor_domain;
  return ExprFeatureDetector<TD>::has_project(expr) &&
         !Exp1ConstEval<TD>::eval(expr).has_value();
}

template <class D>
detail::LinearExecutionPlan choose_adaptive_scc_backends(
    const detail::LinearSccPlan<D> &structure,
    const std::vector<
        std::pair<Symbol, E1<typename TensorSemiringTraits<D>::tensor_domain>>>
        &rhs_tensor,
    const NewtonRoundSetup<D> &setup) {
  detail::LinearExecutionPlan execution;
  execution.sccs.resize(structure.infos.size());
  const bool projection_fragment_supported =
      tensor_supports_projection_equations<D>();
  for (std::size_t sid = 0; sid < structure.infos.size(); ++sid) {
    const auto &info = structure.infos[sid];
    auto &decision = execution.sccs[sid];
    decision.tensor_available = setup.tensor_available;
    decision.tensor_admissible = setup.tensor_admissible;
    decision.tensor_laws_validated = setup.tensor_laws_validated;
    decision.tensor_projection_fragment_supported =
        projection_fragment_supported;
    for (int idx : info.members) {
      if (setup.tensor_laws_validated) {
        decision.tensor_projection_sensitive =
            decision.tensor_projection_sensitive ||
            tensor_expr_is_projection_sensitive<D>(
                rhs_tensor[static_cast<std::size_t>(idx)].second);
      }
    }

    const bool tensor_candidate = info.is_cyclic && info.has_lcfl_structure;
    if (tensor_candidate) {
      if (!setup.tensor_available) {
        decision.tensor_fallback = true;
        decision.tensor_fallback_reason =
            detail::TensorFallbackReason::TensorUnavailable;
      } else if (!setup.tensor_admissible) {
        decision.tensor_fallback = true;
        decision.tensor_fallback_reason =
            detail::TensorFallbackReason::TensorNotPaperAdmissible;
      } else if (!setup.tensor_laws_validated) {
        decision.tensor_fallback = true;
        decision.tensor_fallback_reason =
            detail::TensorFallbackReason::TensorLawsNotValidated;
      } else if (decision.tensor_projection_sensitive &&
                 !projection_fragment_supported) {
        decision.tensor_fallback = true;
        decision.tensor_fallback_reason =
            detail::TensorFallbackReason::ProjectionFragmentUnsupported;
      } else {
        decision.tensor_eligible = true;
      }
    }

    if (info.members.size() == 1 && !info.has_self_loop) {
      decision.backend = detail::SccBackend::Direct;
    } else if (decision.tensor_eligible) {
      decision.backend = detail::SccBackend::Tensor;
    } else {
      decision.backend = detail::SccBackend::Worklist;
    }
  }
  return execution;
}

template <class D>
bool solve_linear_direct_component(
    const std::vector<std::pair<Symbol, E1<D>>> &rhs, const int idx,
    const detail::LinearSccPlan<D> &plan, std::deque<DomVal<D>> &values,
    std::vector<DomVal<D>> &init, long &steps) {
  const long max_steps = domain_max_linear_steps<D>();
  if (max_steps >= 0 && steps >= max_steps)
    return false;
  ++steps;
  auto lookup = [&](const Symbol &sym) -> const DomVal<D> & {
    return values[static_cast<std::size_t>(plan.sym_to_idx.at(sym))];
  };
  auto value = I1<D>::evalWithLookup(false, lookup,
                                     rhs[static_cast<std::size_t>(idx)].second);
  values[static_cast<std::size_t>(idx)] = value;
  init[static_cast<std::size_t>(idx)] = std::move(value);
  return true;
}

template <class D>
std::vector<DomVal<D>> solve_linear_tensor_component(
    bool verbose, const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    const std::vector<
        std::pair<Symbol, E1<typename TensorSemiringTraits<D>::tensor_domain>>>
        &rhs_tensor,
    const detail::LinearSccPlan<D> &plan, const std::vector<int> &scc,
    const std::deque<DomVal<D>> &base_values) {
  using TD = typename TensorSemiringTraits<D>::tensor_domain;
  std::vector<std::pair<Symbol, E1<D>>> local_rhs;
  std::vector<std::pair<Symbol, E1<TD>>> local_rhs_tensor;
  std::vector<DomVal<D>> init;
  local_rhs.reserve(scc.size());
  local_rhs_tensor.reserve(scc.size());
  init.reserve(scc.size());
  for (int idx : scc) {
    local_rhs.push_back(rhs[static_cast<std::size_t>(idx)]);
    local_rhs_tensor.push_back(rhs_tensor[static_cast<std::size_t>(idx)]);
    init.push_back(base_values[static_cast<std::size_t>(idx)]);
  }
  (void)plan;
  return solve_linear_tensor_paper_impl<D>(verbose, local_rhs, local_rhs_tensor,
                                           std::move(init));
}

template <class D>
std::vector<DomVal<D>> solve_linear_adaptive_scc_from_plan(
    bool verbose, const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    const std::vector<
        std::pair<Symbol, E1<typename TensorSemiringTraits<D>::tensor_domain>>>
        &rhs_tensor,
    std::vector<DomVal<D>> init, const detail::LinearSccPlan<D> &plan,
    const detail::LinearExecutionPlan &execution) {
  std::deque<DomVal<D>> values = detail::make_dense_value_buffer<D>(init);
  long steps = 0;
  npa_note_adaptive_scc_used();

  for (int sid : plan.scc_order) {
    const auto &decision = execution.sccs[static_cast<std::size_t>(sid)];
    const auto &scc = plan.sccs[static_cast<std::size_t>(sid)];
    if (decision.tensor_fallback)
      npa_note_adaptive_scc_tensor_fallback(1);

    switch (decision.backend) {
    case detail::SccBackend::Direct:
      npa_note_adaptive_scc_direct(1);
      if (!solve_linear_direct_component<D>(rhs, scc.front(), plan, values,
                                            init, steps)) {
        npa_note_linear_limit_hit();
        return init;
      }
      break;
    case detail::SccBackend::Worklist:
      npa_note_adaptive_scc_worklist(1);
      if (!detail::solve_linear_scc_serial_component<D>(plan, rhs, scc, values,
                                                        init, steps)) {
        npa_note_linear_limit_hit();
        return init;
      }
      break;
    case detail::SccBackend::Tensor: {
      npa_note_adaptive_scc_tensor(1);
      auto component_values = solve_linear_tensor_component<D>(
          verbose, rhs, rhs_tensor, plan, scc, values);
      for (std::size_t pos = 0; pos < scc.size(); ++pos) {
        const auto idx = static_cast<std::size_t>(scc[pos]);
        values[idx] = component_values[pos];
        init[idx] = std::move(component_values[pos]);
      }
      break;
    }
    }
  }

  return init;
}

template <class D>
std::vector<DomVal<D>> solve_linear_adaptive_scc_impl(
    bool verbose, const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    const std::vector<
        std::pair<Symbol, E1<typename TensorSemiringTraits<D>::tensor_domain>>>
        &rhs_tensor,
    std::vector<DomVal<D>> init, const NewtonRoundSetup<D> &setup) {
  auto plan = detail::build_linear_scc_plan<D>(rhs);
  auto execution = choose_adaptive_scc_backends<D>(plan, rhs_tensor, setup);
  return solve_linear_adaptive_scc_from_plan<D>(
      verbose, rhs, rhs_tensor, std::move(init), plan, execution);
}

template <class D>
std::vector<std::pair<Symbol, DomVal<D>>>
run_newton_iteration(bool verbose,
                     const std::vector<std::pair<Symbol, E0<D>>> &eqns,
                     const std::vector<std::pair<Symbol, DomVal<D>>> &binds,
                     LinearStrategy linStrat = LinearStrategy::SCC) {
  using V = DomVal<D>;
  auto setup = build_newton_round_setup<D>(eqns, binds, linStrat);
  const bool use_tensor =
      setup.tensor_laws_validated && setup.has_lcfl_structure;
  if (linStrat == LinearStrategy::TensorProduct && verbose) {
    if (!TensorSemiringTraits<D>::available()) {
      std::cerr << "[tensor] tensor traits unavailable for domain; "
                   "falling back to SCC\n";
    } else if (!setup.has_lcfl_structure) {
      std::cerr << "[tensor] linearized system is already left-linear; "
                   "falling back to SCC\n";
    } else if (!TensorSemiringTraits<D>::paper_admissible()) {
      std::cerr << "[tensor] tensor traits are not paper-admissible; "
                   "falling back to SCC\n";
    } else if (!tensor_paper_laws_validated<D>()) {
      std::cerr << "[tensor] tensor traits did not pass/declare paper-law "
                   "validation; falling back to SCC\n";
    }
  }

  std::vector<V> init(use_tensor ? setup.rhs_tensor.size() : setup.rhs.size(),
                      D::zero()),
      delta;
  if (linStrat == LinearStrategy::Naive) {
    delta = fix_vec<D>(verbose, init, [&](const std::vector<V> &cur) {
      std::unordered_map<Symbol, V> env;
      for (size_t i = 0; i < cur.size(); ++i)
        env.insert_or_assign(setup.rhs[i].first, cur[i]);
      std::vector<V> nxt;
      nxt.reserve(setup.rhs.size());
      for (auto &p : setup.rhs)
        nxt.push_back(I1<D>::eval(false, env, p.second));
      return nxt;
    });
  } else if (linStrat == LinearStrategy::SCC) {
    delta = solve_linear_scc_impl<D>(verbose, setup.rhs, init);
  } else if (linStrat == LinearStrategy::AdaptiveScc) {
    delta = solve_linear_adaptive_scc_impl<D>(verbose, setup.rhs,
                                              setup.rhs_tensor, init, setup);
  } else if (use_tensor) {
    delta = solve_linear_tensor_paper_impl<D>(verbose, setup.rhs,
                                              setup.rhs_tensor, init);
  } else {
    delta = solve_linear_scc_impl<D>(verbose, setup.rhs, init);
  }

  std::vector<std::pair<Symbol, V>> out;
  out.reserve(binds.size());
  for (size_t i = 0; i < binds.size(); ++i) {
    V upd = delta[i];
    V nxt = D::idempotent ? upd : D::combine(binds[i].second, upd);
    out.emplace_back(binds[i].first, nxt);
  }
  return out;
}
} // namespace detail

/// Newton iteration: one round = f(ν) plus least solution of Df|ν(X)+δ = X.
/// δ = f(ν)−ν (or f(ν) when idempotent); Δ = solve linear system; ν' = ν⊕Δ.
///
/// This is the paper-faithful core when the domain uses exact equality and the
/// selected linear solver reaches the least solution without hitting any
/// bounding hooks. `approx_equal`, bounded fixpoint/linear hooks, or explicit
/// outer caps intentionally move the result into the approximate mode recorded
/// in `Stat`. Tensor mode is only used through paper-admissible traits;
/// otherwise the implementation deliberately falls back to the base solver.
template <class D> struct NewtonIter {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  static std::vector<std::pair<Symbol, V>> init(const std::vector<Eqn> &eqns) {
    return detail::build_newton_initial_values<D>(eqns);
  }
  static std::vector<std::pair<Symbol, V>>
  run(bool verbose, const std::vector<Eqn> &eqns,
      const std::vector<std::pair<Symbol, V>> &binds,
      LinearStrategy linStrat = LinearStrategy::SCC) {
    return detail::run_newton_iteration<D>(verbose, eqns, binds, linStrat);
  }
};

template <class D> struct NPASolver {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;

private:
  static std::pair<std::vector<std::pair<Symbol, V>>, Stat>
  solveOnce(const std::vector<Eqn> &eqns, bool verbose, int max,
            LinearStrategy linear_strategy, DomainContractMode contract_mode,
            ConvergencePolicy convergence_policy) {
    SolveOptions options;
    options.verbose = verbose;
    options.max_iterations = max;
    options.linear_strategy = linear_strategy;
    options.contract_mode = contract_mode;
    options.convergence_policy = convergence_policy;
    SolveContext<D> context(std::move(options));
    const bool checks_run = contract_mode != DomainContractMode::Off;
    const bool contract_ok =
        !checks_run || run_basic_domain_contract_checks<D>(verbose);
    if (contract_mode == DomainContractMode::Strict)
      require_domain_contract(contract_ok);
    auto result = iterate_until_stable(
        NewtonIter<D>::init(eqns),
        [&](const std::vector<std::pair<Symbol, V>> &current) {
          return NewtonIter<D>::run(verbose, eqns, current, linear_strategy);
        },
        [](const std::vector<std::pair<Symbol, V>> &lhs,
           const std::vector<std::pair<Symbol, V>> &rhs) {
          for (std::size_t i = 0; i < lhs.size(); ++i)
            if (!domain_equal<D>(lhs[i].second, rhs[i].second))
              return false;
          return true;
        },
        max, verbose);

    if (!result.stabilized && max >= 0) {
      npa_note_outer_limit_hit();
      if (verbose)
        std::cerr << "[conv] hit outer iteration cap=" << max << "\n";
    }

    Stat &stats = context.stats;
    stats.iters = result.iterations;
    stats.time = result.seconds;
    stats.hit_limit = npa_limit_hit();
    stats.hit_outer_limit = npa_hit_outer_limit();
    stats.hit_linear_limit = npa_hit_linear_limit();
    stats.hit_fixpoint_limit = npa_hit_fixpoint_limit();
    stats.equation_count = static_cast<int>(eqns.size());
    stats.requested_max_iters = max;
    stats.effective_max_iters = max;
    stats.linear_strategy = linear_strategy;
    stats.convergence_policy = convergence_policy;
    stats.used_approx_equal =
        DomainHasApproxEqual<D>::value &&
        convergence_policy == ConvergencePolicy::DomainDefault;
    const auto adaptive_stats = npa_adaptive_scc_solve_stats();
    stats.adaptive_scc_used = adaptive_stats.used;
    stats.adaptive_scc_direct_count = adaptive_stats.direct_count;
    stats.adaptive_scc_worklist_count = adaptive_stats.worklist_count;
    stats.adaptive_scc_tensor_count = adaptive_stats.tensor_count;
    stats.adaptive_scc_tensor_fallback_count =
        adaptive_stats.tensor_fallback_count;
    stats.converged =
        result.stabilized && !stats.hit_limit && !stats.used_approx_equal;
    stats.domain_contract_checks_run = checks_run;
    stats.domain_contract_checks_failed = checks_run && !contract_ok;
    return {std::move(result.value), std::move(stats)};
  }

public:
  static std::pair<std::vector<std::pair<Symbol, V>>, Stat> solve(
      const std::vector<Eqn> &eqns, bool verbose = false, int max = -1,
      LinearStrategy linStrat = LinearStrategy::SCC,
      DomainContractMode contractMode = DomainContractMode::Off,
      ConvergencePolicy convergencePolicy = ConvergencePolicy::DomainDefault) {
    NPA_REQUIRE_DOMAIN(D);
    const auto validated = validate_equation_system<D>(eqns);
    (void)validated;
    // JACM (Esparza et al.) shows: for idempotent + commutative semirings,
    // Newton terminates after at most n iterations for a system of n equations.
    // We only apply this bound when the domain explicitly declares
    // commutativity. If that declared contract is insufficient in practice,
    // we continue uncapped rather than silently returning a bounded result.
    int effective_max = max;
    const bool auto_cap =
        effective_max < 0 && D::idempotent && domain_commutative_extend<D>();
    if (auto_cap) {
      effective_max = static_cast<int>(eqns.size());
    }
    auto res = solveOnce(eqns, verbose, effective_max, linStrat, contractMode,
                         convergencePolicy);
    res.second.used_auto_n_cap = auto_cap;
    res.second.effective_max_iters = effective_max;
    if (auto_cap && !res.second.converged) {
      if (verbose)
        std::cerr << "[conv] automatic n-iteration bound was insufficient; "
                     "continuing without the cap\n";
      res = solveOnce(eqns, verbose, -1, linStrat, contractMode,
                      convergencePolicy);
      res.second.used_auto_n_cap = true;
      res.second.retried_without_auto_n_cap = true;
    }
    return res;
  }
};

} // namespace npa

#endif // NPA_NPA_SOLVER_H

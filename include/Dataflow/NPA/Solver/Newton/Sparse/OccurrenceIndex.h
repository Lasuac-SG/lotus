#ifndef NPA_NEWTON_SPARSE_OCCURRENCE_INDEX_H
#define NPA_NEWTON_SPARSE_OCCURRENCE_INDEX_H

/**
 * \file
 * \brief Lazy DAG analysis and filtered differentiation for sparse Newton.
 *
 * The sparse plan stores only the source-to-target dependency skeleton up
 * front. A target's expression DAG is indexed when demand first reaches it.
 * Each round then computes source influence once per shared Exp0 node instead
 * of enumerating root-to-leaf occurrence paths. Joining contexts at a shared
 * node is deliberately conservative: it can retain extra terms, but cannot
 * remove a potentially non-zero derivative contribution.
 */

#include "Dataflow/NPA/Core/Expr/Eval.h"
#include "Dataflow/NPA/Solver/EquationSystem.h"
#include "Dataflow/NPA/Solver/Newton/Errors.h"
#include "Dataflow/NPA/Solver/Options.h"
#include "Dataflow/NPA/Solver/Statistics.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace npa {

namespace detail {

template <class D> struct DomainHasSparseLeftZeroAnnihilator {
  template <class T>
  static auto test(int)
      -> decltype(T::sparse_npa_zero_left_annihilator, std::true_type{});
  template <class> static std::false_type test(...);

  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasSparseRightZeroAnnihilator {
  template <class T>
  static auto test(int)
      -> decltype(T::sparse_npa_zero_right_annihilator, std::true_type{});
  template <class> static std::false_type test(...);

  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> inline bool sparse_left_zero_annihilator(std::true_type) {
  return D::sparse_npa_zero_left_annihilator;
}

template <class D> inline bool sparse_left_zero_annihilator(std::false_type) {
  return false;
}

template <class D> inline bool sparse_right_zero_annihilator(std::true_type) {
  return D::sparse_npa_zero_right_annihilator;
}

template <class D> inline bool sparse_right_zero_annihilator(std::false_type) {
  return false;
}

} // namespace detail

/// Domain customization point used by sparse zero-context analysis.
template <class D> struct SparseNewtonZeroOracle {
  using V = DomVal<D>;

  static bool isZero(const V &value) {
    return domain_exact_equal<D>(value, D::zero());
  }

  static bool leftZeroAnnihilates() {
    return detail::sparse_left_zero_annihilator<D>(
        std::integral_constant<
            bool, detail::DomainHasSparseLeftZeroAnnihilator<D>::value>{});
  }

  static bool rightZeroAnnihilates() {
    return detail::sparse_right_zero_annihilator<D>(
        std::integral_constant<
            bool, detail::DomainHasSparseRightZeroAnnihilator<D>::value>{});
  }

  static bool leftMultiplyIsZeroMap(const V &coefficient) {
    return isZero(coefficient) && leftZeroAnnihilates();
  }

  static bool rightMultiplyIsZeroMap(const V &coefficient) {
    return isZero(coefficient) && rightZeroAnnihilates();
  }
};

namespace detail {

template <class D> struct SparseRoundMaterialization {
  std::vector<std::pair<Symbol, E1<D>>> rhs;
  std::vector<unsigned> dense_indices;
  NewtonRoundStat stats;
};

template <class D> class SparseNewtonSystem {
public:
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  using Env = typename I0<D>::Environment;
  using SourceSet = std::unordered_set<unsigned>;

  SparseNewtonSystem(const std::vector<Eqn> &equations,
                     const std::vector<std::pair<Symbol, V>> &fixed_seed,
                     const ValidatedEquationSystem &validated)
      : symbol_to_index_(validated.symbol_to_index),
        targets_by_source_(equations.size()), target_indexes_(equations.size()) {
    symbols_.reserve(equations.size());
    equations_.reserve(equations.size());
    seed_.reserve(equations.size());
    seed_support_.assign(equations.size(), false);
    for (std::size_t target = 0; target < equations.size(); ++target) {
      symbols_.push_back(equations[target].first);
      equations_.push_back(equations[target].second);
      seed_.push_back(fixed_seed[target].second);
      seed_support_[target] =
          !SparseNewtonZeroOracle<D>::isZero(fixed_seed[target].second);
      for (unsigned source : validated.dependencies[target])
        targets_by_source_[source].push_back(static_cast<unsigned>(target));
    }
    static_active_ = discoverStaticReachability();
  }

  std::size_t occurrenceCount() const { return indexed_leaf_count_; }
  double occurrenceIndexTime() const { return lazy_index_time_; }

  SparseRoundMaterialization<D>
  buildRound(const std::vector<std::pair<Symbol, V>> &binds,
             NewtonRoundStrategy strategy) const {
    std::unordered_map<Symbol, V> nu;
    nu.reserve(binds.size());
    for (const auto &binding : binds)
      nu.insert_or_assign(binding.first, binding.second);

    const auto discovery_start = std::chrono::steady_clock::now();
    std::vector<bool> active(symbols_.size(), false);
    std::vector<std::shared_ptr<const InfluenceResult>> influences(
        symbols_.size());
    long queries = 0;
    long retained = 0;

    auto ensureInfluence = [&](unsigned target) -> const InfluenceResult & {
      if (!influences[target]) {
        InfluenceContext context;
        influences[target] = analyzeExpression(
            equations_[target], nu, {}, {}, context, 0);
      }
      return *influences[target];
    };

    if (strategy == NewtonRoundStrategy::Static) {
      active = static_active_;
      for (std::size_t target = 0; target < active.size(); ++target) {
        if (!active[target])
          continue;
        const TargetIndex &index = ensureTargetIndex(target);
        for (std::size_t source = 0; source < active.size(); ++source) {
          if (active[source])
            retained += index.source_counts[source];
        }
      }
    } else {
      std::deque<unsigned> worklist;
      for (std::size_t source = 0; source < seed_support_.size(); ++source) {
        if (!seed_support_[source])
          continue;
        active[source] = true;
        worklist.push_back(static_cast<unsigned>(source));
      }

      while (!worklist.empty()) {
        const unsigned source = worklist.front();
        worklist.pop_front();
        for (unsigned target : targets_by_source_[source]) {
          const TargetIndex &index = ensureTargetIndex(target);
          const long source_occurrences = index.source_counts[source];
          queries += source_occurrences;

          const InfluenceResult &influence = ensureInfluence(target);
          const bool keep = strategy == NewtonRoundStrategy::AlwaysMaybe ||
                            influence.sources.count(source) != 0;
          if (!keep)
            continue;
          retained += source_occurrences;
          if (!active[target]) {
            active[target] = true;
            worklist.push_back(target);
          }
        }
      }
    }

    SparseRoundMaterialization<D> result;
    result.stats.discovery_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      discovery_start)
            .count();
    result.stats.queried_occurrences = queries;
    result.stats.retained_occurrences = retained;
    result.stats.active_coordinates =
        static_cast<int>(std::count(active.begin(), active.end(), true));

    const auto materialization_start = std::chrono::steady_clock::now();
    result.rhs.reserve(
        static_cast<std::size_t>(result.stats.active_coordinates));
    result.dense_indices.reserve(
        static_cast<std::size_t>(result.stats.active_coordinates));
    long materialized_terms = 0;

    for (std::size_t target = 0; target < symbols_.size(); ++target) {
      if (!active[target])
        continue;

      std::vector<bool> allowed_sources(symbols_.size(), false);
      const InfluenceResult *influence = nullptr;
      if (strategy == NewtonRoundStrategy::Sparse)
        influence = &ensureInfluence(static_cast<unsigned>(target));
      const TargetIndex &index = ensureTargetIndex(target);
      for (std::size_t source = 0; source < symbols_.size(); ++source) {
        allowed_sources[source] =
            active[source] &&
            (!influence || influence->sources.count(source) != 0);
        if (allowed_sources[source])
          materialized_terms += index.source_counts[source];
      }

      FilteredBuildContext build_context;
      auto filtered = buildFilteredDerivative(
          equations_[target], nu, {}, {}, allowed_sources, build_context, 0);
      E1<D> rhs = Exp1<D>::term(seed_[target]);
      if (filtered->derivative) {
        rhs = Exp1<D>::add(std::move(rhs),
                           filtered->derivative);
      }
      result.rhs.emplace_back(symbols_[target], std::move(rhs));
      result.dense_indices.push_back(static_cast<unsigned>(target));
    }

    result.stats.materialized_derivative_terms = materialized_terms;
    result.stats.materialization_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      materialization_start)
            .count();
    return result;
  }

private:
  struct NodeScopeKey {
    const Exp0<D> *expression = nullptr;
    std::size_t scope = 0;

    bool operator==(const NodeScopeKey &other) const {
      return expression == other.expression && scope == other.scope;
    }
  };

  struct NodeScopeKeyHash {
    std::size_t operator()(const NodeScopeKey &key) const {
      const std::size_t pointer_hash =
          std::hash<const Exp0<D> *>{}(key.expression);
      return pointer_hash ^
             (key.scope + static_cast<std::size_t>(0x9e3779b9) +
              (pointer_hash << 6) + (pointer_hash >> 2));
    }
  };

  struct TargetIndex {
    explicit TargetIndex(std::size_t source_count)
        : source_counts(source_count, 0) {}

    std::vector<long> source_counts;
    std::size_t leaf_count = 0;
  };

  struct IndexContext {
    std::unordered_set<NodeScopeKey, NodeScopeKeyHash> visited;
    std::size_t next_scope = 1;
  };

  struct InfluenceResult {
    V value;
    SourceSet sources;
  };

  using SharedInfluence = std::shared_ptr<const InfluenceResult>;

  struct InfluenceContext {
    std::unordered_map<NodeScopeKey, SharedInfluence, NodeScopeKeyHash> memo;
    std::size_t next_scope = 1;
  };

  struct FilteredDerivativeResult {
    V value;
    E1<D> derivative;
  };

  using SharedFiltered = std::shared_ptr<const FilteredDerivativeResult>;

  struct FilteredBuildContext {
    std::unordered_map<NodeScopeKey, SharedFiltered, NodeScopeKeyHash> memo;
    std::size_t next_scope = 1;
  };

  enum class ZeroOperationKind { LeftMultiply, RightMultiply };

  struct ZeroOperation {
    ZeroOperationKind kind;
    const V *coefficient = nullptr;
  };

  std::unordered_map<Symbol, unsigned> symbol_to_index_;
  std::vector<Symbol> symbols_;
  std::vector<E0<D>> equations_;
  std::vector<V> seed_;
  std::vector<bool> seed_support_;
  std::vector<bool> static_active_;
  std::vector<std::vector<unsigned>> targets_by_source_;
  mutable std::vector<std::optional<TargetIndex>> target_indexes_;
  mutable std::size_t indexed_leaf_count_ = 0;
  mutable double lazy_index_time_ = 0.0;

  unsigned sourceIndex(const Symbol &symbol) const {
    return symbol_to_index_.at(symbol);
  }

  std::vector<bool> discoverStaticReachability() const {
    std::vector<bool> active = seed_support_;
    std::deque<unsigned> worklist;
    for (std::size_t source = 0; source < active.size(); ++source) {
      if (active[source])
        worklist.push_back(static_cast<unsigned>(source));
    }
    while (!worklist.empty()) {
      const unsigned source = worklist.front();
      worklist.pop_front();
      for (unsigned target : targets_by_source_[source]) {
        if (!active[target]) {
          active[target] = true;
          worklist.push_back(target);
        }
      }
    }
    return active;
  }

  const TargetIndex &ensureTargetIndex(std::size_t target) const {
    if (target_indexes_[target])
      return *target_indexes_[target];

    const auto start = std::chrono::steady_clock::now();
    TargetIndex index(symbols_.size());
    IndexContext context;
    indexExpression(equations_[target], {}, index, context, 0);
    indexed_leaf_count_ += index.leaf_count;
    target_indexes_[target].emplace(std::move(index));
    lazy_index_time_ +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    return *target_indexes_[target];
  }

  void noteLeaf(const Symbol &symbol, TargetIndex &index) const {
    ++index.source_counts[sourceIndex(symbol)];
    ++index.leaf_count;
  }

  void indexExpression(const E0<D> &expression,
                       const std::unordered_set<Symbol> &bound,
                       TargetIndex &index, IndexContext &context,
                       std::size_t scope) const {
    if (!context.visited.insert({expression.get(), scope}).second)
      return;

    using K = typename Exp0<D>::K;
    switch (expression->k) {
    case K::Term:
    case K::Bound:
      return;
    case K::Seq:
    case K::Project:
      indexExpression(expression->t, bound, index, context, scope);
      return;
    case K::Mul:
    case K::Cond:
    case K::Ndet:
      indexExpression(expression->t1, bound, index, context, scope);
      indexExpression(expression->t2, bound, index, context, scope);
      return;
    case K::Call:
      indexExpression(expression->t, bound, index, context, scope);
      noteLeaf(expression->sym, index);
      return;
    case K::Hole:
      noteLeaf(expression->sym, index);
      return;
    case K::Concat:
      indexExpression(expression->t1, bound, index, context, scope);
      if (!bound.count(expression->sym))
        noteLeaf(expression->sym, index);
      indexExpression(expression->t2, bound, index, context, scope);
      return;
    case K::Star:
    case K::Mu: {
      auto body_bound = bound;
      body_bound.insert(expression->sym);
      const std::size_t body_scope = context.next_scope++;
      indexExpression(expression->t, body_bound, index, context, body_scope);
      return;
    }
    }
  }

  static void mergeSources(SourceSet &destination, const SourceSet &source) {
    destination.insert(source.begin(), source.end());
  }

  static void applyZeroOperation(std::optional<V> &constant,
                                 const ZeroOperation &operation) {
    if (operation.kind == ZeroOperationKind::LeftMultiply) {
      if (constant) {
        constant = D::extend(*operation.coefficient, *constant);
      } else if (SparseNewtonZeroOracle<D>::leftMultiplyIsZeroMap(
                     *operation.coefficient)) {
        constant = D::zero();
      }
      return;
    }

    if (constant) {
      constant = D::extend(*constant, *operation.coefficient);
    } else if (SparseNewtonZeroOracle<D>::rightMultiplyIsZeroMap(
                   *operation.coefficient)) {
      constant = D::zero();
    }
  }

  static bool contextIsZero(
      std::initializer_list<ZeroOperation> operations) {
    std::optional<V> constant;
    for (const ZeroOperation &operation : operations)
      applyZeroOperation(constant, operation);
    return constant && SparseNewtonZeroOracle<D>::isZero(*constant);
  }

  static void filterSources(SourceSet &sources,
                            std::initializer_list<ZeroOperation> operations) {
    if (contextIsZero(operations))
      sources.clear();
  }

  static ZeroOperation left(const V &coefficient) {
    return {ZeroOperationKind::LeftMultiply, &coefficient};
  }

  static ZeroOperation right(const V &coefficient) {
    return {ZeroOperationKind::RightMultiply, &coefficient};
  }

  SharedInfluence analyzeExpression(
      const E0<D> &expression, const std::unordered_map<Symbol, V> &nu,
      const Env &env, const std::unordered_set<Symbol> &bound,
      InfluenceContext &context, std::size_t scope) const {
    const NodeScopeKey key{expression.get(), scope};
    auto cached = context.memo.find(key);
    if (cached != context.memo.end())
      return cached->second;

    SharedInfluence result = analyzeExpressionUncached(
        expression, nu, env, bound, context, scope);
    context.memo.emplace(key, result);
    return result;
  }

  SharedInfluence analyzeExpressionUncached(
      const E0<D> &expression, const std::unordered_map<Symbol, V> &nu,
      const Env &env, const std::unordered_set<Symbol> &bound,
      InfluenceContext &context, std::size_t scope) const {
    using K = typename Exp0<D>::K;
    switch (expression->k) {
    case K::Term:
      return std::make_shared<InfluenceResult>(
          InfluenceResult{expression->c, {}});
    case K::Seq: {
      SharedInfluence child = analyzeExpression(
          expression->t, nu, env, bound, context, scope);
      SourceSet sources = child->sources;
      filterSources(sources, {left(expression->c)});
      return std::make_shared<InfluenceResult>(InfluenceResult{
          D::extend(expression->c, child->value), std::move(sources)});
    }
    case K::Mul: {
      SharedInfluence lhs = analyzeExpression(
          expression->t1, nu, env, bound, context, scope);
      SharedInfluence rhs = analyzeExpression(
          expression->t2, nu, env, bound, context, scope);
      SourceSet sources = lhs->sources;
      filterSources(sources, {right(rhs->value)});
      SourceSet rhs_sources = rhs->sources;
      filterSources(rhs_sources, {left(lhs->value)});
      mergeSources(sources, rhs_sources);
      return std::make_shared<InfluenceResult>(InfluenceResult{
          D::extend(lhs->value, rhs->value), std::move(sources)});
    }
    case K::Call: {
      SharedInfluence argument = analyzeExpression(
          expression->t, nu, env, bound, context, scope);
      const V &callee = nu.at(expression->sym);
      SourceSet sources = argument->sources;
      filterSources(sources, {left(callee)});
      if (!contextIsZero({right(argument->value)}))
        sources.insert(sourceIndex(expression->sym));
      return std::make_shared<InfluenceResult>(InfluenceResult{
          D::extend(callee, argument->value), std::move(sources)});
    }
    case K::Cond: {
      SharedInfluence then_result = analyzeExpression(
          expression->t1, nu, env, bound, context, scope);
      SharedInfluence else_result = analyzeExpression(
          expression->t2, nu, env, bound, context, scope);
      SourceSet sources =
          expression->phi ? then_result->sources : else_result->sources;
      return std::make_shared<InfluenceResult>(InfluenceResult{
          D::condCombine(expression->phi, then_result->value,
                         else_result->value),
          std::move(sources)});
    }
    case K::Ndet: {
      SharedInfluence lhs = analyzeExpression(
          expression->t1, nu, env, bound, context, scope);
      SharedInfluence rhs = analyzeExpression(
          expression->t2, nu, env, bound, context, scope);
      SourceSet sources = lhs->sources;
      mergeSources(sources, rhs->sources);
      return std::make_shared<InfluenceResult>(InfluenceResult{
          D::ndetCombine(lhs->value, rhs->value), std::move(sources)});
    }
    case K::Project: {
      SharedInfluence child = analyzeExpression(
          expression->t, nu, env, bound, context, scope);
      return std::make_shared<InfluenceResult>(InfluenceResult{
          domain_project<D>(child->value), child->sources});
    }
    case K::Hole:
      return std::make_shared<InfluenceResult>(InfluenceResult{
          nu.at(expression->sym), {sourceIndex(expression->sym)}});
    case K::Bound:
      return std::make_shared<InfluenceResult>(
          InfluenceResult{env.at(expression->sym), {}});
    case K::Concat: {
      SharedInfluence lhs = analyzeExpression(
          expression->t1, nu, env, bound, context, scope);
      SharedInfluence rhs = analyzeExpression(
          expression->t2, nu, env, bound, context, scope);
      auto local = env.find(expression->sym);
      const V &middle =
          local == env.end() ? nu.at(expression->sym) : local->second;

      SourceSet sources = lhs->sources;
      const V middle_right = D::extend(middle, rhs->value);
      filterSources(sources, {right(middle_right)});
      SourceSet rhs_sources = rhs->sources;
      filterSources(rhs_sources, {left(middle), left(lhs->value)});
      mergeSources(sources, rhs_sources);
      if (!bound.count(expression->sym) &&
          !contextIsZero({left(lhs->value), right(rhs->value)})) {
        sources.insert(sourceIndex(expression->sym));
      }
      return std::make_shared<InfluenceResult>(InfluenceResult{
          D::extend(lhs->value, D::extend(middle, rhs->value)),
          std::move(sources)});
    }
    case K::Star: {
      V star_value = I0<D>::evalWithEnvironment(nu, env, expression);
      if constexpr (DomainHasStar<D>::value) {
        if (E0<D> operand = matchSemiringStarOperand<D>(expression)) {
          SharedInfluence body = analyzeExpression(
              operand, nu, env, bound, context, scope);
          SourceSet sources = body->sources;
          filterSources(sources,
                        {right(star_value), left(star_value)});
          return std::make_shared<InfluenceResult>(InfluenceResult{
              std::move(star_value), std::move(sources)});
        }
      }

      Env body_env = env;
      body_env.insert_or_assign(expression->sym, star_value);
      auto body_bound = bound;
      body_bound.insert(expression->sym);
      const std::size_t body_scope = context.next_scope++;
      SharedInfluence body = analyzeExpression(
          expression->t, nu, body_env, body_bound, context, body_scope);
      SourceSet sources = body->sources;
      filterSources(sources, {right(star_value), left(star_value)});
      return std::make_shared<InfluenceResult>(InfluenceResult{
          std::move(star_value), std::move(sources)});
    }
    case K::Mu:
      throw UnsupportedNewtonMuError{};
    }
    return nullptr;
  }

  static E1<D> combineLinear(E1<D> lhs, E1<D> rhs) {
    if (!lhs)
      return rhs;
    if (!rhs)
      return lhs;
    return Exp1<D>::add(std::move(lhs), std::move(rhs));
  }

  SharedFiltered buildFilteredDerivative(
      const E0<D> &expression, const std::unordered_map<Symbol, V> &nu,
      const Env &env, const std::unordered_set<Symbol> &bound,
      const std::vector<bool> &allowed_sources,
      FilteredBuildContext &context, std::size_t scope) const {
    const NodeScopeKey key{expression.get(), scope};
    auto cached = context.memo.find(key);
    if (cached != context.memo.end())
      return cached->second;
    SharedFiltered result = buildFilteredDerivativeUncached(
        expression, nu, env, bound, allowed_sources, context, scope);
    context.memo.emplace(key, result);
    return result;
  }

  SharedFiltered buildFilteredDerivativeUncached(
      const E0<D> &expression, const std::unordered_map<Symbol, V> &nu,
      const Env &env, const std::unordered_set<Symbol> &bound,
      const std::vector<bool> &allowed_sources,
      FilteredBuildContext &context, std::size_t scope) const {
    using K = typename Exp0<D>::K;
    switch (expression->k) {
    case K::Term:
      return std::make_shared<FilteredDerivativeResult>(
          FilteredDerivativeResult{expression->c, nullptr});
    case K::Seq: {
      SharedFiltered child = buildFilteredDerivative(
          expression->t, nu, env, bound, allowed_sources, context, scope);
      E1<D> derivative = child->derivative
                             ? Exp1<D>::seq(expression->c, child->derivative)
                             : nullptr;
      return std::make_shared<FilteredDerivativeResult>(
          FilteredDerivativeResult{
              D::extend(expression->c, child->value),
              std::move(derivative)});
    }
    case K::Mul: {
      SharedFiltered lhs = buildFilteredDerivative(
          expression->t1, nu, env, bound, allowed_sources, context, scope);
      SharedFiltered rhs = buildFilteredDerivative(
          expression->t2, nu, env, bound, allowed_sources, context, scope);
      E1<D> lhs_term = lhs->derivative
                           ? Exp1<D>::seqR(lhs->derivative, rhs->value)
                           : nullptr;
      E1<D> rhs_term = rhs->derivative
                           ? Exp1<D>::seq(lhs->value, rhs->derivative)
                           : nullptr;
      return std::make_shared<FilteredDerivativeResult>(
          FilteredDerivativeResult{
              D::extend(lhs->value, rhs->value),
              combineLinear(std::move(lhs_term), std::move(rhs_term))});
    }
    case K::Call: {
      SharedFiltered argument = buildFilteredDerivative(
          expression->t, nu, env, bound, allowed_sources, context, scope);
      const V &callee = nu.at(expression->sym);
      E1<D> argument_term =
          argument->derivative
              ? Exp1<D>::seq(callee, argument->derivative)
              : nullptr;
      E1<D> callee_term;
      if (allowed_sources[sourceIndex(expression->sym)]) {
        callee_term = Exp1<D>::call(expression->sym, argument->value);
      }
      return std::make_shared<FilteredDerivativeResult>(
          FilteredDerivativeResult{
              D::extend(callee, argument->value),
              combineLinear(std::move(argument_term),
                            std::move(callee_term))});
    }
    case K::Cond: {
      SharedFiltered then_result = buildFilteredDerivative(
          expression->t1, nu, env, bound, allowed_sources, context, scope);
      SharedFiltered else_result = buildFilteredDerivative(
          expression->t2, nu, env, bound, allowed_sources, context, scope);
      E1<D> derivative = expression->phi ? then_result->derivative
                                         : else_result->derivative;
      return std::make_shared<FilteredDerivativeResult>(
          FilteredDerivativeResult{
              D::condCombine(expression->phi, then_result->value,
                             else_result->value),
              std::move(derivative)});
    }
    case K::Ndet: {
      SharedFiltered lhs = buildFilteredDerivative(
          expression->t1, nu, env, bound, allowed_sources, context, scope);
      SharedFiltered rhs = buildFilteredDerivative(
          expression->t2, nu, env, bound, allowed_sources, context, scope);
      return std::make_shared<FilteredDerivativeResult>(
          FilteredDerivativeResult{
              D::ndetCombine(lhs->value, rhs->value),
              combineLinear(lhs->derivative, rhs->derivative)});
    }
    case K::Project: {
      SharedFiltered child = buildFilteredDerivative(
          expression->t, nu, env, bound, allowed_sources, context, scope);
      E1<D> derivative = child->derivative
                             ? Exp1<D>::project(child->derivative)
                             : nullptr;
      return std::make_shared<FilteredDerivativeResult>(
          FilteredDerivativeResult{domain_project<D>(child->value),
                                   std::move(derivative)});
    }
    case K::Hole: {
      E1<D> derivative;
      if (allowed_sources[sourceIndex(expression->sym)])
        derivative = Exp1<D>::hole(expression->sym);
      return std::make_shared<FilteredDerivativeResult>(
          FilteredDerivativeResult{nu.at(expression->sym),
                                   std::move(derivative)});
    }
    case K::Bound:
      return std::make_shared<FilteredDerivativeResult>(
          FilteredDerivativeResult{env.at(expression->sym), nullptr});
    case K::Concat: {
      SharedFiltered lhs = buildFilteredDerivative(
          expression->t1, nu, env, bound, allowed_sources, context, scope);
      SharedFiltered rhs = buildFilteredDerivative(
          expression->t2, nu, env, bound, allowed_sources, context, scope);
      auto local = env.find(expression->sym);
      const V &middle =
          local == env.end() ? nu.at(expression->sym) : local->second;
      E1<D> lhs_term =
          lhs->derivative
              ? Exp1<D>::seqR(lhs->derivative,
                              D::extend(middle, rhs->value))
              : nullptr;
      E1<D> middle_term;
      if (!bound.count(expression->sym) &&
          allowed_sources[sourceIndex(expression->sym)]) {
        middle_term = Exp1<D>::concat(Exp1<D>::term(lhs->value),
                                      expression->sym,
                                      Exp1<D>::term(rhs->value));
      }
      E1<D> rhs_term =
          rhs->derivative
              ? Exp1<D>::seq(
                    lhs->value, Exp1<D>::seq(middle, rhs->derivative))
              : nullptr;
      E1<D> derivative = combineLinear(
          combineLinear(std::move(lhs_term), std::move(middle_term)),
          std::move(rhs_term));
      return std::make_shared<FilteredDerivativeResult>(
          FilteredDerivativeResult{
              D::extend(lhs->value, D::extend(middle, rhs->value)),
              std::move(derivative)});
    }
    case K::Star: {
      V star_value = I0<D>::evalWithEnvironment(nu, env, expression);
      if constexpr (DomainHasStar<D>::value) {
        if (E0<D> operand = matchSemiringStarOperand<D>(expression)) {
          SharedFiltered body = buildFilteredDerivative(
              operand, nu, env, bound, allowed_sources, context, scope);
          E1<D> derivative =
              body->derivative
                  ? Exp1<D>::seq(
                        star_value,
                        Exp1<D>::seqR(body->derivative, star_value))
                  : nullptr;
          return std::make_shared<FilteredDerivativeResult>(
              FilteredDerivativeResult{std::move(star_value),
                                       std::move(derivative)});
        }
      }

      Env body_env = env;
      body_env.insert_or_assign(expression->sym, star_value);
      auto body_bound = bound;
      body_bound.insert(expression->sym);
      const std::size_t body_scope = context.next_scope++;
      SharedFiltered body = buildFilteredDerivative(
          expression->t, nu, body_env, body_bound, allowed_sources, context,
          body_scope);
      E1<D> derivative =
          body->derivative
              ? Exp1<D>::seq(star_value,
                             Exp1<D>::seqR(body->derivative, star_value))
              : nullptr;
      return std::make_shared<FilteredDerivativeResult>(
          FilteredDerivativeResult{std::move(star_value),
                                   std::move(derivative)});
    }
    case K::Mu:
      throw UnsupportedNewtonMuError{};
    }
    return nullptr;
  }
};

} // namespace detail
} // namespace npa

#endif // NPA_NEWTON_SPARSE_OCCURRENCE_INDEX_H

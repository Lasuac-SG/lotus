#ifndef NPA_NEWTON_SPARSE_OCCURRENCE_INDEX_H
#define NPA_NEWTON_SPARSE_OCCURRENCE_INDEX_H

/**
 * \file
 * \brief Source-indexed derivative occurrences for sparse Newton rounds.
 *
 * The index names each free equation-variable occurrence in an Exp0 tree.
 * A round follows occurrences reachable from supp(F(0)), evaluates only their
 * context recipes, then performs one value-and-filtered-derivative traversal
 * per active target instead of rebuilding every retained root-to-leaf path.
 */

#include "Dataflow/NPA/Core/Expr/Eval.h"
#include "Dataflow/NPA/Solver/EquationSystem.h"
#include "Dataflow/NPA/Solver/Newton/Errors.h"
#include "Dataflow/NPA/Solver/Options.h"
#include "Dataflow/NPA/Solver/Statistics.h"

#include <algorithm>
#include <chrono>
#include <deque>
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

/// Domain customization point used by sparse occurrence-context evaluation.
///
/// A domain opts into annihilator pruning by declaring either or both of
/// `sparse_npa_zero_left_annihilator` and
/// `sparse_npa_zero_right_annihilator`.  Undeclared laws conservatively yield
/// Maybe.  A domain may specialize this class to use a different exact zero
/// representation while preserving the one-sided Zero => zero-map contract.
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

  /// Whether a -> coefficient ⊗ a is the zero map.  Specializations may
  /// recognize additional domain-specific zero transformers.
  static bool leftMultiplyIsZeroMap(const V &coefficient) {
    return isZero(coefficient) && leftZeroAnnihilates();
  }

  /// Whether a -> a ⊗ coefficient is the zero map.  Specializations may
  /// recognize additional domain-specific zero transformers.
  static bool rightMultiplyIsZeroMap(const V &coefficient) {
    return isZero(coefficient) && rightZeroAnnihilates();
  }
};

namespace detail {

enum class SparseOccurrenceStepKind {
  SeqBody,
  MulLeft,
  MulRight,
  CallArgument,
  CondThen,
  CondElse,
  NdetLeft,
  NdetRight,
  ProjectBody,
  ConcatLeft,
  ConcatRight,
  StarBody,
};

enum class SparseOccurrenceLeafKind {
  Hole,
  CallCallee,
  ConcatMiddle,
};

template <class D> struct SparseOccurrencePath {
  SparseOccurrenceStepKind kind;
  E0<D> node;
  std::shared_ptr<const SparseOccurrencePath<D>> parent;

  SparseOccurrencePath(
      SparseOccurrenceStepKind step_kind, E0<D> step_node,
      std::shared_ptr<const SparseOccurrencePath<D>> parent_path)
      : kind(step_kind), node(std::move(step_node)),
        parent(std::move(parent_path)) {}
};

template <class D> struct SparseDerivativeOccurrence {
  unsigned source = 0;
  unsigned target = 0;
  Symbol source_symbol;
  SparseOccurrenceLeafKind leaf_kind = SparseOccurrenceLeafKind::Hole;
  E0<D> leaf;
  std::shared_ptr<const SparseOccurrencePath<D>> path;
};

/// One-time, source-bucketed occurrence table.  Paths share their prefixes.
template <class D> class SparseDerivativeOccurrenceIndex {
public:
  using Eqn = std::pair<Symbol, E0<D>>;
  using Occurrence = SparseDerivativeOccurrence<D>;
  using Path = SparseOccurrencePath<D>;
  using PathPtr = std::shared_ptr<const Path>;

  SparseDerivativeOccurrenceIndex(const std::vector<Eqn> &equations,
                                  const ValidatedEquationSystem &validated)
      : symbol_to_index_(validated.symbol_to_index),
        by_source_(equations.size()), target_ranges_(equations.size()) {
    for (std::size_t target = 0; target < equations.size(); ++target) {
      target_ranges_[target].first = occurrences_.size();
      indexExpression(equations[target].second, static_cast<unsigned>(target),
                      nullptr, {});
      target_ranges_[target].second = occurrences_.size();
    }
  }

  const Occurrence &occurrence(std::size_t id) const {
    return occurrences_.at(id);
  }

  const std::vector<std::size_t> &from(unsigned source) const {
    return by_source_.at(source);
  }

  std::size_t size() const { return occurrences_.size(); }

  std::pair<std::size_t, std::size_t> targetRange(unsigned target) const {
    return target_ranges_.at(target);
  }

private:
  std::unordered_map<Symbol, unsigned> symbol_to_index_;
  std::vector<Occurrence> occurrences_;
  std::vector<std::vector<std::size_t>> by_source_;
  std::vector<std::pair<std::size_t, std::size_t>> target_ranges_;

  PathPtr descend(PathPtr parent, SparseOccurrenceStepKind kind,
                  const E0<D> &node) {
    return std::make_shared<Path>(kind, node, std::move(parent));
  }

  void addOccurrence(const Symbol &source_symbol, unsigned target,
                     SparseOccurrenceLeafKind leaf_kind, const E0<D> &leaf,
                     PathPtr path) {
    const unsigned source = symbol_to_index_.at(source_symbol);
    const std::size_t id = occurrences_.size();
    occurrences_.push_back(
        {source, target, source_symbol, leaf_kind, leaf, std::move(path)});
    by_source_[source].push_back(id);
  }

  void indexExpression(const E0<D> &expr, unsigned target, PathPtr path,
                       const std::unordered_set<Symbol> &bound) {
    using K = typename Exp0<D>::K;
    switch (expr->k) {
    case K::Term:
    case K::Bound:
      return;
    case K::Seq:
      indexExpression(expr->t, target,
                      descend(path, SparseOccurrenceStepKind::SeqBody, expr),
                      bound);
      return;
    case K::Mul:
      indexExpression(expr->t1, target,
                      descend(path, SparseOccurrenceStepKind::MulLeft, expr),
                      bound);
      indexExpression(expr->t2, target,
                      descend(path, SparseOccurrenceStepKind::MulRight, expr),
                      bound);
      return;
    case K::Call:
      indexExpression(
          expr->t, target,
          descend(path, SparseOccurrenceStepKind::CallArgument, expr), bound);
      addOccurrence(expr->sym, target, SparseOccurrenceLeafKind::CallCallee,
                    expr, path);
      return;
    case K::Cond:
      indexExpression(expr->t1, target,
                      descend(path, SparseOccurrenceStepKind::CondThen, expr),
                      bound);
      indexExpression(expr->t2, target,
                      descend(path, SparseOccurrenceStepKind::CondElse, expr),
                      bound);
      return;
    case K::Ndet:
      indexExpression(expr->t1, target,
                      descend(path, SparseOccurrenceStepKind::NdetLeft, expr),
                      bound);
      indexExpression(expr->t2, target,
                      descend(path, SparseOccurrenceStepKind::NdetRight, expr),
                      bound);
      return;
    case K::Project:
      indexExpression(
          expr->t, target,
          descend(path, SparseOccurrenceStepKind::ProjectBody, expr), bound);
      return;
    case K::Hole:
      addOccurrence(expr->sym, target, SparseOccurrenceLeafKind::Hole, expr,
                    std::move(path));
      return;
    case K::Concat:
      indexExpression(expr->t1, target,
                      descend(path, SparseOccurrenceStepKind::ConcatLeft, expr),
                      bound);
      if (bound.find(expr->sym) == bound.end())
        addOccurrence(expr->sym, target, SparseOccurrenceLeafKind::ConcatMiddle,
                      expr, path);
      indexExpression(
          expr->t2, target,
          descend(path, SparseOccurrenceStepKind::ConcatRight, expr), bound);
      return;
    case K::Star: {
      auto body_bound = bound;
      body_bound.insert(expr->sym);
      indexExpression(expr->t, target,
                      descend(path, SparseOccurrenceStepKind::StarBody, expr),
                      body_bound);
      return;
    }
    case K::Mu: {
      auto body_bound = bound;
      body_bound.insert(expr->sym);
      indexExpression(expr->t, target, path, body_bound);
      return;
    }
    }
  }
};

enum class SparseContextOperationKind {
  LeftMultiply,
  RightMultiply,
  CondThen,
  CondElse,
  Project,
};

template <class D> struct SparseContextOperation {
  using V = DomVal<D>;
  using T = DomTest<D>;

  SparseContextOperationKind kind;
  std::shared_ptr<const V> coefficient;
  T condition{};

  static SparseContextOperation left(std::shared_ptr<const V> value) {
    SparseContextOperation op{SparseContextOperationKind::LeftMultiply};
    op.coefficient = std::move(value);
    return op;
  }

  static SparseContextOperation right(std::shared_ptr<const V> value) {
    SparseContextOperation op{SparseContextOperationKind::RightMultiply};
    op.coefficient = std::move(value);
    return op;
  }

  static SparseContextOperation condThen(T value) {
    SparseContextOperation op{SparseContextOperationKind::CondThen};
    op.condition = std::move(value);
    return op;
  }

  static SparseContextOperation condElse(T value) {
    SparseContextOperation op{SparseContextOperationKind::CondElse};
    op.condition = std::move(value);
    return op;
  }

  static SparseContextOperation project() {
    return SparseContextOperation{SparseContextOperationKind::Project};
  }

private:
  explicit SparseContextOperation(SparseContextOperationKind op_kind)
      : kind(op_kind) {}
};

template <class D> struct SparsePreparedPath;

template <class D> struct SparsePreparedOccurrence {
  using V = DomVal<D>;

  SparseOccurrenceLeafKind leaf_kind = SparseOccurrenceLeafKind::Hole;
  Symbol source_symbol;
  std::shared_ptr<const V> leaf_left;
  std::shared_ptr<const V> leaf_right;
  std::shared_ptr<const SparsePreparedPath<D>> outer_path;
};

template <class D> struct SparsePreparedPath {
  using Env = typename I0<D>::Environment;
  using Op = SparseContextOperation<D>;

  std::optional<Op> first_operation;
  std::optional<Op> second_operation;
  std::shared_ptr<const SparsePreparedPath> parent;
  std::shared_ptr<const Env> environment;
};

template <class D> struct SparseRoundMaterialization {
  std::vector<std::pair<Symbol, E1<D>>> rhs;
  std::vector<unsigned> dense_indices;
  NewtonRoundStat stats;
};

/// Reusable sparse-Newton plan for one validated equation system.
template <class D> class SparseNewtonSystem {
public:
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  using Env = typename I0<D>::Environment;
  using SharedValue = std::shared_ptr<const V>;
  using EvalCache = std::unordered_map<const Exp0<D> *, SharedValue>;
  using PreparedPath = SparsePreparedPath<D>;
  using SharedPreparedPath = std::shared_ptr<const PreparedPath>;
  using PathCache =
      std::unordered_map<const SparseOccurrencePath<D> *, SharedPreparedPath>;
  using Occurrence = SparseDerivativeOccurrence<D>;
  using Prepared = SparsePreparedOccurrence<D>;
  using Op = SparseContextOperation<D>;

  struct RetainedLeafKey {
    const Exp0<D> *expression = nullptr;
    SparseOccurrenceLeafKind kind = SparseOccurrenceLeafKind::Hole;

    bool operator==(const RetainedLeafKey &other) const {
      return expression == other.expression && kind == other.kind;
    }
  };

  struct RetainedLeafKeyHash {
    std::size_t operator()(const RetainedLeafKey &key) const {
      const std::size_t pointerHash =
          std::hash<const Exp0<D> *>{}(key.expression);
      return pointerHash ^
             (static_cast<std::size_t>(key.kind) +
              static_cast<std::size_t>(0x9e3779b9) + (pointerHash << 6) +
              (pointerHash >> 2));
    }
  };

  using RetainedLeafSet =
      std::unordered_set<RetainedLeafKey, RetainedLeafKeyHash>;

private:
  struct FilteredBuildContext;

public:

  SparseNewtonSystem(const std::vector<Eqn> &equations,
                     const std::vector<std::pair<Symbol, V>> &fixed_seed,
                     const ValidatedEquationSystem &validated)
      : index_(equations, validated) {
    symbols_.reserve(equations.size());
    equations_.reserve(equations.size());
    seed_.reserve(equations.size());
    seed_support_.assign(equations.size(), false);
    for (std::size_t i = 0; i < equations.size(); ++i) {
      symbols_.push_back(equations[i].first);
      equations_.push_back(equations[i].second);
      seed_.push_back(fixed_seed[i].second);
      seed_support_[i] =
          !SparseNewtonZeroOracle<D>::isZero(fixed_seed[i].second);
    }
    static_active_ = discoverStaticReachability();
  }

  std::size_t occurrenceCount() const { return index_.size(); }

  SparseRoundMaterialization<D>
  buildRound(const std::vector<std::pair<Symbol, V>> &binds,
             NewtonRoundStrategy strategy) const {
    std::unordered_map<Symbol, V> nu;
    nu.reserve(binds.size());
    for (const auto &binding : binds)
      nu.insert_or_assign(binding.first, binding.second);

    const auto discovery_start = std::chrono::steady_clock::now();
    std::vector<bool> active(symbols_.size(), false);
    std::vector<std::size_t> retained;
    EvalCache evaluation_cache;
    PathCache path_cache;
    const auto empty_environment = std::make_shared<const Env>();
    std::vector<bool> target_values_cached(symbols_.size(), false);
    long queries = 0;

    if (strategy == NewtonRoundStrategy::Static) {
      active = static_active_;
      retainStaticOccurrences(active, retained);
    } else {
      std::deque<unsigned> worklist;
      for (std::size_t i = 0; i < seed_support_.size(); ++i) {
        if (!seed_support_[i])
          continue;
        active[i] = true;
        worklist.push_back(static_cast<unsigned>(i));
      }

      while (!worklist.empty()) {
        const unsigned source = worklist.front();
        worklist.pop_front();
        for (std::size_t id : index_.from(source)) {
          ++queries;
          bool is_zero = false;
          if (strategy == NewtonRoundStrategy::Sparse ||
              strategy == NewtonRoundStrategy::AlwaysMaybe) {
            cacheTargetValues(index_.occurrence(id).target, nu,
                              evaluation_cache, target_values_cached);
            Prepared occurrence =
                prepareOccurrence(index_.occurrence(id), nu, evaluation_cache,
                                  path_cache, empty_environment);
            const bool oracle_zero = occurrenceIsZero(occurrence);
            is_zero = strategy == NewtonRoundStrategy::Sparse && oracle_zero;
          }
          if (is_zero)
            continue;
          retained.push_back(id);
          const unsigned target = index_.occurrence(id).target;
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
    result.stats.retained_occurrences = static_cast<long>(retained.size());
    result.stats.active_coordinates =
        static_cast<int>(std::count(active.begin(), active.end(), true));

    const auto materialization_start = std::chrono::steady_clock::now();
    std::vector<RetainedLeafSet> retained_leaves(symbols_.size());
    for (std::size_t id : retained) {
      const Occurrence &occurrence = index_.occurrence(id);
      retained_leaves[occurrence.target].insert(
          {occurrence.leaf.get(), occurrence.leaf_kind});
    }

    result.rhs.reserve(
        static_cast<std::size_t>(result.stats.active_coordinates));
    result.dense_indices.reserve(
        static_cast<std::size_t>(result.stats.active_coordinates));
    for (std::size_t target = 0; target < symbols_.size(); ++target) {
      if (!active[target])
        continue;
      FilteredBuildContext build_context;
      auto filtered = buildFilteredDerivative(
          equations_[target], nu, {}, {}, retained_leaves[target],
          evaluation_cache, strategy != NewtonRoundStrategy::Static,
          build_context, 0);
      E1<D> derivative = std::move(filtered.derivative);
      E1<D> rhs = Exp1<D>::term(seed_[target]);
      if (derivative)
        rhs = Exp1<D>::add(std::move(rhs), std::move(derivative));
      result.rhs.emplace_back(symbols_[target], std::move(rhs));
      result.dense_indices.push_back(static_cast<unsigned>(target));
    }
    result.stats.materialized_derivative_terms =
        static_cast<long>(retained.size());
    result.stats.materialization_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      materialization_start)
            .count();
    return result;
  }

private:
  SparseDerivativeOccurrenceIndex<D> index_;
  std::vector<Symbol> symbols_;
  std::vector<E0<D>> equations_;
  std::vector<V> seed_;
  std::vector<bool> seed_support_;
  std::vector<bool> static_active_;

  std::vector<bool> discoverStaticReachability() const {
    std::vector<bool> active = seed_support_;
    std::deque<unsigned> worklist;
    for (std::size_t i = 0; i < active.size(); ++i)
      if (active[i])
        worklist.push_back(static_cast<unsigned>(i));
    while (!worklist.empty()) {
      const unsigned source = worklist.front();
      worklist.pop_front();
      for (std::size_t id : index_.from(source)) {
        const unsigned target = index_.occurrence(id).target;
        if (!active[target]) {
          active[target] = true;
          worklist.push_back(target);
        }
      }
    }
    return active;
  }

  void retainStaticOccurrences(const std::vector<bool> &active,
                               std::vector<std::size_t> &retained) const {
    for (std::size_t source = 0; source < active.size(); ++source) {
      if (!active[source])
        continue;
      const auto &bucket = index_.from(static_cast<unsigned>(source));
      retained.insert(retained.end(), bucket.begin(), bucket.end());
    }
  }

  static SharedValue evaluate(const std::unordered_map<Symbol, V> &nu,
                              const Env &env, const E0<D> &expr,
                              EvalCache &cache) {
    if (!env.empty())
      return std::make_shared<V>(I0<D>::evalWithEnvironment(nu, env, expr));
    auto found = cache.find(expr.get());
    if (found != cache.end())
      return found->second;
    SharedValue value =
        std::make_shared<V>(I0<D>::evalWithEnvironment(nu, env, expr));
    cache.emplace(expr.get(), value);
    return value;
  }

  static SharedValue middleValue(const std::unordered_map<Symbol, V> &nu,
                                 const Env &env, const Symbol &symbol) {
    auto local = env.find(symbol);
    return std::make_shared<V>(local == env.end() ? nu.at(symbol)
                                                  : local->second);
  }

  void cacheTargetValues(unsigned target,
                         const std::unordered_map<Symbol, V> &nu,
                         EvalCache &cache,
                         std::vector<bool> &target_values_cached) const {
    if (target_values_cached[target])
      return;
    auto context = std::make_shared<typename I0<D>::EvaluationContext>();
    (void)I0<D>::evalWithContext(nu, {}, equations_[target], *context);
    for (const auto &entry : context->values) {
      SharedValue value(context, &entry.second);
      cache.emplace(entry.first, std::move(value));
    }
    target_values_cached[target] = true;
  }

  static E1<D> combineLinear(E1<D> lhs, E1<D> rhs) {
    if (!lhs)
      return rhs;
    if (!rhs)
      return lhs;
    return Exp1<D>::add(std::move(lhs), std::move(rhs));
  }

  template <class Compute>
  static V reuseOrComputeValue(const E0<D> &expr, EvalCache &cache,
                               bool reuse_cached_values, Compute compute) {
    if (reuse_cached_values) {
      auto found = cache.find(expr.get());
      if (found != cache.end())
        return *found->second;
    }
    return compute();
  }

  static bool leafIsRetained(const RetainedLeafSet &retained_leaves,
                             const E0<D> &expression,
                             SparseOccurrenceLeafKind kind) {
    return retained_leaves.count({expression.get(), kind}) != 0;
  }

  struct FilteredDerivativeResult {
    V value;
    E1<D> derivative;
  };

  struct FilteredMemoKey {
    const Exp0<D> *expression = nullptr;
    std::size_t scope = 0;

    bool operator==(const FilteredMemoKey &other) const {
      return expression == other.expression && scope == other.scope;
    }
  };

  struct FilteredMemoKeyHash {
    std::size_t operator()(const FilteredMemoKey &key) const {
      const std::size_t pointerHash =
          std::hash<const Exp0<D> *>{}(key.expression);
      return pointerHash ^ (key.scope + static_cast<std::size_t>(0x9e3779b9) +
                            (pointerHash << 6) + (pointerHash >> 2));
    }
  };

  struct FilteredBuildContext {
    std::unordered_map<FilteredMemoKey, FilteredDerivativeResult,
                       FilteredMemoKeyHash>
        memo;
    std::size_t next_scope = 1;
  };

  FilteredDerivativeResult buildFilteredDerivative(
      const E0<D> &expr, const std::unordered_map<Symbol, V> &nu,
      const Env &env, const std::unordered_set<Symbol> &bound,
      const RetainedLeafSet &retained_leaves, EvalCache &evaluation_cache,
      bool reuse_cached_values, FilteredBuildContext &build_context,
      std::size_t scope) const {
    const FilteredMemoKey key{expr.get(), scope};
    auto cached = build_context.memo.find(key);
    if (cached != build_context.memo.end())
      return cached->second;
    FilteredDerivativeResult result = buildFilteredDerivativeUncached(
        expr, nu, env, bound, retained_leaves, evaluation_cache,
        reuse_cached_values, build_context, scope);
    build_context.memo.emplace(key, result);
    return result;
  }

  FilteredDerivativeResult buildFilteredDerivativeUncached(
      const E0<D> &expr, const std::unordered_map<Symbol, V> &nu,
      const Env &env, const std::unordered_set<Symbol> &bound,
      const RetainedLeafSet &retained_leaves, EvalCache &evaluation_cache,
      bool reuse_cached_values, FilteredBuildContext &build_context,
      std::size_t scope) const {
    using K = typename Exp0<D>::K;
    switch (expr->k) {
    case K::Term:
      return {reuseOrComputeValue(expr, evaluation_cache, reuse_cached_values,
                                  [&]() { return expr->c; }),
              nullptr};
    case K::Seq: {
      auto child = buildFilteredDerivative(
          expr->t, nu, env, bound, retained_leaves, evaluation_cache,
          reuse_cached_values, build_context, scope);
      V value = reuseOrComputeValue(
          expr, evaluation_cache, reuse_cached_values,
          [&]() { return D::extend(expr->c, child.value); });
      E1<D> derivative =
          child.derivative ? Exp1<D>::seq(expr->c, std::move(child.derivative))
                           : nullptr;
      return {std::move(value), std::move(derivative)};
    }
    case K::Mul: {
      auto lhs = buildFilteredDerivative(
          expr->t1, nu, env, bound, retained_leaves, evaluation_cache,
          reuse_cached_values, build_context, scope);
      auto rhs = buildFilteredDerivative(
          expr->t2, nu, env, bound, retained_leaves, evaluation_cache,
          reuse_cached_values, build_context, scope);
      V value = reuseOrComputeValue(
          expr, evaluation_cache, reuse_cached_values,
          [&]() { return D::extend(lhs.value, rhs.value); });
      E1<D> lhs_term;
      E1<D> rhs_term;
      if (lhs.derivative)
        lhs_term = Exp1<D>::seqR(std::move(lhs.derivative), rhs.value);
      if (rhs.derivative)
        rhs_term = Exp1<D>::seq(lhs.value, std::move(rhs.derivative));
      return {std::move(value),
              combineLinear(std::move(lhs_term), std::move(rhs_term))};
    }
    case K::Call: {
      auto argument = buildFilteredDerivative(
          expr->t, nu, env, bound, retained_leaves, evaluation_cache,
          reuse_cached_values, build_context, scope);
      V value = reuseOrComputeValue(
          expr, evaluation_cache, reuse_cached_values,
          [&]() { return D::extend(nu.at(expr->sym), argument.value); });
      E1<D> argument_term;
      if (argument.derivative)
        argument_term =
            Exp1<D>::seq(nu.at(expr->sym), std::move(argument.derivative));
      E1<D> callee_term;
      if (leafIsRetained(retained_leaves, expr,
                         SparseOccurrenceLeafKind::CallCallee))
        callee_term = Exp1<D>::call(expr->sym, argument.value);
      return {std::move(value),
              combineLinear(std::move(argument_term), std::move(callee_term))};
    }
    case K::Cond: {
      auto then_result = buildFilteredDerivative(
          expr->t1, nu, env, bound, retained_leaves, evaluation_cache,
          reuse_cached_values, build_context, scope);
      auto else_result = buildFilteredDerivative(
          expr->t2, nu, env, bound, retained_leaves, evaluation_cache,
          reuse_cached_values, build_context, scope);
      V value = reuseOrComputeValue(
          expr, evaluation_cache, reuse_cached_values, [&]() {
            return D::condCombine(expr->phi, then_result.value,
                                  else_result.value);
          });
      E1<D> then_derivative = std::move(then_result.derivative);
      E1<D> else_derivative = std::move(else_result.derivative);
      if (!then_derivative && !else_derivative)
        return {std::move(value), nullptr};
      if (!then_derivative)
        then_derivative = Exp1<D>::term(D::zero());
      if (!else_derivative)
        else_derivative = Exp1<D>::term(D::zero());
      return {std::move(value),
              Exp1<D>::cond(expr->phi, std::move(then_derivative),
                            std::move(else_derivative))};
    }
    case K::Ndet: {
      auto lhs = buildFilteredDerivative(
          expr->t1, nu, env, bound, retained_leaves, evaluation_cache,
          reuse_cached_values, build_context, scope);
      auto rhs = buildFilteredDerivative(
          expr->t2, nu, env, bound, retained_leaves, evaluation_cache,
          reuse_cached_values, build_context, scope);
      V value = reuseOrComputeValue(
          expr, evaluation_cache, reuse_cached_values,
          [&]() { return D::ndetCombine(lhs.value, rhs.value); });
      return {std::move(value), combineLinear(std::move(lhs.derivative),
                                              std::move(rhs.derivative))};
    }
    case K::Project: {
      auto child = buildFilteredDerivative(
          expr->t, nu, env, bound, retained_leaves, evaluation_cache,
          reuse_cached_values, build_context, scope);
      V value =
          reuseOrComputeValue(expr, evaluation_cache, reuse_cached_values,
                              [&]() { return domain_project<D>(child.value); });
      E1<D> derivative = child.derivative
                             ? Exp1<D>::project(std::move(child.derivative))
                             : nullptr;
      return {std::move(value), std::move(derivative)};
    }
    case K::Hole:
      return {reuseOrComputeValue(expr, evaluation_cache, reuse_cached_values,
                                  [&]() { return nu.at(expr->sym); }),
              leafIsRetained(retained_leaves, expr,
                             SparseOccurrenceLeafKind::Hole)
                  ? Exp1<D>::hole(expr->sym)
                  : nullptr};
    case K::Bound:
      return {reuseOrComputeValue(expr, evaluation_cache, reuse_cached_values,
                                  [&]() { return env.at(expr->sym); }),
              nullptr};
    case K::Concat: {
      auto lhs = buildFilteredDerivative(
          expr->t1, nu, env, bound, retained_leaves, evaluation_cache,
          reuse_cached_values, build_context, scope);
      const bool middle_retained =
          bound.find(expr->sym) == bound.end() &&
          leafIsRetained(retained_leaves, expr,
                         SparseOccurrenceLeafKind::ConcatMiddle);
      auto rhs = buildFilteredDerivative(
          expr->t2, nu, env, bound, retained_leaves, evaluation_cache,
          reuse_cached_values, build_context, scope);
      auto local = env.find(expr->sym);
      V middle = local == env.end() ? nu.at(expr->sym) : local->second;
      V value = reuseOrComputeValue(
          expr, evaluation_cache, reuse_cached_values,
          [&]() { return D::extend(lhs.value, D::extend(middle, rhs.value)); });
      E1<D> lhs_term;
      if (lhs.derivative)
        lhs_term = Exp1<D>::seqR(std::move(lhs.derivative),
                                 D::extend(middle, rhs.value));
      E1<D> middle_term;
      if (middle_retained)
        middle_term = Exp1<D>::concat(Exp1<D>::term(lhs.value), expr->sym,
                                      Exp1<D>::term(rhs.value));
      E1<D> rhs_term;
      if (rhs.derivative)
        rhs_term = Exp1<D>::seq(
            lhs.value, Exp1<D>::seq(middle, std::move(rhs.derivative)));
      return {std::move(value),
              combineLinear(
                  combineLinear(std::move(lhs_term), std::move(middle_term)),
                  std::move(rhs_term))};
    }
    case K::Star: {
      V star_value = reuseOrComputeValue(
          expr, evaluation_cache, reuse_cached_values,
          [&]() { return I0<D>::evalWithEnvironment(nu, env, expr); });
      if constexpr (DomainHasStar<D>::value) {
        if (E0<D> operand = matchSemiringStarOperand<D>(expr)) {
          auto body = buildFilteredDerivative(
              operand, nu, env, bound, retained_leaves, evaluation_cache,
              reuse_cached_values, build_context, scope);
          E1<D> derivative =
              body.derivative
                  ? Exp1<D>::seq(
                        star_value,
                        Exp1<D>::seqR(std::move(body.derivative), star_value))
                  : nullptr;
          return {std::move(star_value), std::move(derivative)};
        }
      }
      Env body_environment = env;
      body_environment.insert_or_assign(expr->sym, star_value);
      auto body_bound = bound;
      body_bound.insert(expr->sym);
      const std::size_t body_scope = build_context.next_scope++;
      auto body = buildFilteredDerivative(
          expr->t, nu, body_environment, body_bound, retained_leaves,
          evaluation_cache, reuse_cached_values, build_context, body_scope);
      E1<D> derivative =
          body.derivative
              ? Exp1<D>::seq(
                    star_value,
                    Exp1<D>::seqR(std::move(body.derivative), star_value))
              : nullptr;
      return {std::move(star_value), std::move(derivative)};
    }
    case K::Mu:
      throw UnsupportedNewtonMuError{};
    }
    return {D::zero(), nullptr};
  }

  SharedPreparedPath
  preparePath(const std::shared_ptr<const SparseOccurrencePath<D>> &path,
              const std::unordered_map<Symbol, V> &nu,
              EvalCache &evaluation_cache, PathCache &path_cache,
              const std::shared_ptr<const Env> &empty_environment) const {
    if (!path)
      return nullptr;
    auto found = path_cache.find(path.get());
    if (found != path_cache.end())
      return found->second;

    SharedPreparedPath parent = preparePath(path->parent, nu, evaluation_cache,
                                            path_cache, empty_environment);
    std::shared_ptr<const Env> environment =
        parent ? parent->environment : empty_environment;
    auto prepared = std::make_shared<PreparedPath>();
    prepared->parent = std::move(parent);
    prepared->environment = environment;
    const E0<D> &node = path->node;

    switch (path->kind) {
    case SparseOccurrenceStepKind::SeqBody:
      prepared->first_operation = Op::left(std::make_shared<V>(node->c));
      break;
    case SparseOccurrenceStepKind::MulLeft:
      prepared->first_operation =
          Op::right(evaluate(nu, *environment, node->t2, evaluation_cache));
      break;
    case SparseOccurrenceStepKind::MulRight:
      prepared->first_operation =
          Op::left(evaluate(nu, *environment, node->t1, evaluation_cache));
      break;
    case SparseOccurrenceStepKind::CallArgument:
      prepared->first_operation =
          Op::left(std::make_shared<V>(nu.at(node->sym)));
      break;
    case SparseOccurrenceStepKind::CondThen:
      prepared->first_operation = Op::condThen(node->phi);
      break;
    case SparseOccurrenceStepKind::CondElse:
      prepared->first_operation = Op::condElse(node->phi);
      break;
    case SparseOccurrenceStepKind::NdetLeft:
    case SparseOccurrenceStepKind::NdetRight:
      break;
    case SparseOccurrenceStepKind::ProjectBody:
      prepared->first_operation = Op::project();
      break;
    case SparseOccurrenceStepKind::ConcatLeft: {
      SharedValue middle = middleValue(nu, *environment, node->sym);
      SharedValue right =
          evaluate(nu, *environment, node->t2, evaluation_cache);
      prepared->first_operation =
          Op::right(std::make_shared<V>(D::extend(*middle, *right)));
      break;
    }
    case SparseOccurrenceStepKind::ConcatRight: {
      SharedValue left = evaluate(nu, *environment, node->t1, evaluation_cache);
      SharedValue middle = middleValue(nu, *environment, node->sym);
      prepared->first_operation = Op::left(std::move(middle));
      prepared->second_operation = Op::left(std::move(left));
      break;
    }
    case SparseOccurrenceStepKind::StarBody: {
      SharedValue star = evaluate(nu, *environment, node, evaluation_cache);
      prepared->first_operation = Op::right(star);
      prepared->second_operation = Op::left(star);
      auto body_environment = std::make_shared<Env>(*environment);
      body_environment->insert_or_assign(node->sym, *star);
      prepared->environment = std::move(body_environment);
      break;
    }
    }

    SharedPreparedPath result = prepared;
    path_cache.emplace(path.get(), result);
    return result;
  }

  Prepared
  prepareOccurrence(const Occurrence &occurrence,
                    const std::unordered_map<Symbol, V> &nu,
                    EvalCache &evaluation_cache, PathCache &path_cache,
                    const std::shared_ptr<const Env> &empty_environment) const {
    Prepared prepared;
    prepared.leaf_kind = occurrence.leaf_kind;
    prepared.source_symbol = occurrence.source_symbol;
    prepared.outer_path = preparePath(occurrence.path, nu, evaluation_cache,
                                      path_cache, empty_environment);
    const Env &environment = prepared.outer_path
                                 ? *prepared.outer_path->environment
                                 : *empty_environment;
    if (occurrence.leaf_kind == SparseOccurrenceLeafKind::CallCallee) {
      prepared.leaf_right =
          evaluate(nu, environment, occurrence.leaf->t, evaluation_cache);
    } else if (occurrence.leaf_kind == SparseOccurrenceLeafKind::ConcatMiddle) {
      prepared.leaf_left =
          evaluate(nu, environment, occurrence.leaf->t1, evaluation_cache);
      prepared.leaf_right =
          evaluate(nu, environment, occurrence.leaf->t2, evaluation_cache);
    }
    return prepared;
  }

  static void applyZeroOperation(std::optional<V> &constant, const Op &op) {
    switch (op.kind) {
    case SparseContextOperationKind::LeftMultiply:
      if (constant) {
        constant = D::extend(*op.coefficient, *constant);
      } else if (SparseNewtonZeroOracle<D>::leftMultiplyIsZeroMap(
                     *op.coefficient)) {
        constant = D::zero();
      }
      return;
    case SparseContextOperationKind::RightMultiply:
      if (constant) {
        constant = D::extend(*constant, *op.coefficient);
      } else if (SparseNewtonZeroOracle<D>::rightMultiplyIsZeroMap(
                     *op.coefficient)) {
        constant = D::zero();
      }
      return;
    case SparseContextOperationKind::CondThen:
      if (constant)
        constant = D::condCombine(op.condition, *constant, D::zero());
      return;
    case SparseContextOperationKind::CondElse:
      if (constant)
        constant = D::condCombine(op.condition, D::zero(), *constant);
      return;
    case SparseContextOperationKind::Project:
      if (constant)
        constant = domain_project<D>(*constant);
      return;
    }
  }

  static bool occurrenceIsZero(const Prepared &prepared) {
    std::optional<V> constant;
    if (prepared.leaf_kind == SparseOccurrenceLeafKind::CallCallee) {
      applyZeroOperation(constant, Op::right(prepared.leaf_right));
    } else if (prepared.leaf_kind == SparseOccurrenceLeafKind::ConcatMiddle) {
      applyZeroOperation(constant, Op::left(prepared.leaf_left));
      applyZeroOperation(constant, Op::right(prepared.leaf_right));
    }
    for (SharedPreparedPath path = prepared.outer_path; path;
         path = path->parent) {
      if (path->first_operation)
        applyZeroOperation(constant, *path->first_operation);
      if (path->second_operation)
        applyZeroOperation(constant, *path->second_operation);
    }
    return constant && SparseNewtonZeroOracle<D>::isZero(*constant);
  }
};

} // namespace detail
} // namespace npa

#endif // NPA_NEWTON_SPARSE_OCCURRENCE_INDEX_H

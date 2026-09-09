#pragma once

#include "CFL/InterleavedDyck/SPDS/Semiring.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::spds {

using State = std::size_t;
using Symbol = std::uint64_t;
// Only automaton transitions may carry Epsilon. It is NOT a stack symbol.
inline constexpr Symbol Epsilon = std::numeric_limits<Symbol>::max();

enum class Direction { Post, Pre };
struct Limits {
  std::size_t max_states = 0;
  std::size_t max_transitions = 0;
  std::size_t max_updates = 0;
};
class ResourceLimit : public std::runtime_error {
public:
  explicit ResourceLimit(const std::string &what) : std::runtime_error(what) {}
};
struct Statistics {
  std::size_t states = 0, transitions = 0, updates = 0, processed = 0,
              rules = 0;
  std::uint64_t setup_microseconds = 0;
  std::uint64_t saturation_microseconds = 0;
  std::uint64_t readout_microseconds = 0;
  std::uint64_t projection_microseconds = 0;
};
struct Configuration {
  State control = 0;
  // Top first. The empty vector is a genuinely empty PDS stack.
  std::vector<Symbol> stack;
};
struct Transition {
  State from = 0;
  Symbol label = Epsilon;
  State to = 0;
  bool operator<(const Transition &r) const {
    return std::tie(from, label, to) < std::tie(r.from, r.label, r.to);
  }
  bool operator==(const Transition &r) const {
    return from == r.from && label == r.label && to == r.to;
  }
};
struct TransitionHash {
  std::size_t operator()(const Transition &edge) const {
    std::uint64_t seed = static_cast<std::uint64_t>(edge.from);
    seed ^= static_cast<std::uint64_t>(edge.label) +
            0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    seed ^= static_cast<std::uint64_t>(edge.to) + 0x9e3779b97f4a7c15ULL +
            (seed << 6U) + (seed >> 2U);
    seed ^= seed >> 30U;
    seed *= 0xbf58476d1ce4e5b9ULL;
    seed ^= seed >> 27U;
    seed *= 0x94d049bb133111ebULL;
    seed ^= seed >> 31U;
    return static_cast<std::size_t>(seed);
  }
};
class TransitionIndex {
public:
  static constexpr std::size_t Missing =
      std::numeric_limits<std::size_t>::max();

  template <class Records>
  std::size_t find(const Transition &edge, const Records &records) const {
    if (buckets_.empty())
      return Missing;
    const std::size_t mask = buckets_.size() - 1;
    std::size_t bucket = TransitionHash{}(edge) & mask;
    while (buckets_[bucket] != Missing) {
      const std::size_t id = buckets_[bucket];
      if (records[id].edge == edge)
        return id;
      bucket = (bucket + 1) & mask;
    }
    return Missing;
  }

  template <class Records>
  void insert(const Transition &edge, std::size_t id,
              const Records &records) {
    if (buckets_.empty()) {
      grow(2, records);
    } else if (size_ >= buckets_.size() / 2) {
      if (buckets_.size() >
          std::numeric_limits<std::size_t>::max() / 2)
        throw std::length_error("SPDS transition index overflow");
      grow(buckets_.size() * 2, records);
    }
    place(buckets_, TransitionHash{}(edge), id);
    ++size_;
  }

private:
  static void place(std::vector<std::size_t> &buckets, std::size_t hash,
                    std::size_t id) {
    const std::size_t mask = buckets.size() - 1;
    std::size_t bucket = hash & mask;
    while (buckets[bucket] != Missing)
      bucket = (bucket + 1) & mask;
    buckets[bucket] = id;
  }
  template <class Records>
  void grow(std::size_t capacity, const Records &records) {
    std::vector<std::size_t> replacement(capacity, Missing);
    for (std::size_t id : buckets_)
      if (id != Missing)
        place(replacement, TransitionHash{}(records[id].edge), id);
    buckets_.swap(replacement);
  }
  std::vector<std::size_t> buckets_;
  std::size_t size_ = 0;
};
using RuleKey = std::pair<State, Symbol>;
struct RuleKeyHash {
  std::size_t operator()(const RuleKey &key) const {
    std::size_t seed = std::hash<State>{}(key.first);
    seed ^= std::hash<Symbol>{}(key.second) + 0x9e3779b9U + (seed << 6U) +
            (seed >> 2U);
    return seed;
  }
};
template <class Domain, class = void> struct DomainPreparedWeight {
  struct Type {};
  static constexpr bool available = false;
  static Type prepare(const Domain &, const typename Domain::Weight &) {
    return {};
  }
};
template <class Domain>
struct DomainPreparedWeight<Domain,
                            std::void_t<typename Domain::PreparedWeight>> {
  using Type = typename Domain::PreparedWeight;
  static constexpr bool available = true;
  static Type prepare(const Domain &domain,
                      const typename Domain::Weight &weight) {
    return domain.prepareWeight(weight);
  }
};
template <class Domain, class = void> struct DomainDelta {
  struct Type {};
  static constexpr bool available = false;
};
template <class Domain>
struct DomainDelta<Domain, std::void_t<typename Domain::Delta>> {
  using Type = typename Domain::Delta;
  static constexpr bool available = true;
};

// Unweighted regular seed/target set. States [0,controls) are PDS controls.
// Incoming edges to controls, epsilon edges, and accepting controls are
// allowed: saturation normalizes a private copy before computing post* or pre*.
class RegularSet {
public:
  explicit RegularSet(std::size_t controls)
      : controls_(controls), states_(controls) {}
  State addState() { return states_++; }
  void addFinal(State q) {
    check(q);
    finals_.insert(q);
  }
  void addTransition(State from, Symbol label, State to) {
    check(from);
    check(to);
    edges_.push_back({from, label, to});
  }
  static RegularSet singleton(std::size_t controls, const Configuration &c) {
    if (c.control >= controls)
      throw std::out_of_range("seed control");
    RegularSet result(controls);
    State q = c.control;
    if (c.stack.empty()) {
      result.addFinal(q);
    } else {
      for (Symbol a : c.stack) {
        if (a == Epsilon)
          throw std::invalid_argument("epsilon in seed stack");
        State next = result.addState();
        result.addTransition(q, a, next);
        q = next;
      }
      result.addFinal(q);
    }
    return result;
  }
  std::size_t controls() const { return controls_; }
  std::size_t states() const { return states_; }
  const std::vector<Transition> &transitions() const { return edges_; }
  const std::set<State> &finals() const { return finals_; }

private:
  void check(State q) const {
    if (q >= states_)
      throw std::out_of_range("seed automaton state");
  }
  std::size_t controls_, states_;
  std::vector<Transition> edges_;
  std::set<State> finals_;
};

template <class Domain = BooleanSemiring> class PushdownSystem {
public:
  using Weight = typename Domain::Weight;
  using PreparedWeight = typename DomainPreparedWeight<Domain>::Type;
  static constexpr bool HAS_PREPARED_WEIGHT =
      DomainPreparedWeight<Domain>::available;
  static constexpr std::size_t NO_GENERATED =
      std::numeric_limits<std::size_t>::max();
  enum class RuleKind { Exact, PreserveAny, PushAny };
  struct Rule {
    RuleKind kind;
    State from;
    Symbol top;
    State to;
    std::vector<Symbol> replacement; // [], [a], or [a,b], top first
    Weight weight;
    PreparedWeight prepared;
    std::size_t generated_slot = NO_GENERATED;
  };
  struct RuleIndex {
    std::unordered_map<RuleKey, std::vector<std::size_t>, RuleKeyHash> exact;
    std::vector<std::vector<std::size_t>> wildcard;
    std::vector<std::size_t> initial;
  };
  struct CompiledRuleIndexes {
    RuleIndex post, pre;
    std::unordered_map<RuleKey, std::size_t, RuleKeyHash> generated_slots;
  };
  explicit PushdownSystem(Domain domain = Domain())
      : domain_(std::move(domain)),
        indexes_(std::make_shared<CompiledRuleIndexes>()) {}
  State addControl() {
    auto &indexes = writeIndexes();
    indexes.post.wildcard.emplace_back();
    indexes.pre.wildcard.emplace_back();
    return controls_++;
  }
  void addRule(State from, Symbol top, State to,
               std::vector<Symbol> replacement, const Weight &weight) {
    if (from >= controls_ || to >= controls_)
      throw std::out_of_range("PDS control");
    if (top == Epsilon || replacement.size() > 2 ||
        std::find(replacement.begin(), replacement.end(), Epsilon) !=
            replacement.end())
      throw std::invalid_argument(
          "PDS rule must replace one symbol by at most two");
    // Also catches incompatible weights in domains such as RelationSemiring.
    (void)domain_.combine(domain_.zero(), weight);
    rules_.push_back({RuleKind::Exact, from, top, to,
                      std::move(replacement), weight, prepareWeight(weight)});
    indexRule(rules_.size() - 1);
  }
  void addRule(State from, Symbol top, State to,
               std::vector<Symbol> replacement) {
    addRule(from, top, to, std::move(replacement), domain_.one());
  }
  // Rules quantified over the current top symbol. These compactly represent
  // p X -> q X and p X -> q pushed X for every non-epsilon stack symbol X.
  void addPreserveRule(State from, State to, const Weight &weight) {
    addWildcardRule(RuleKind::PreserveAny, from, to, 0, weight);
  }
  void addPreserveRule(State from, State to) {
    addPreserveRule(from, to, domain_.one());
  }
  void addPushRule(State from, State to, Symbol pushed, const Weight &weight) {
    if (pushed == Epsilon)
      throw std::invalid_argument("PDS push symbol must not be epsilon");
    addWildcardRule(RuleKind::PushAny, from, to, pushed, weight);
  }
  void addPushRule(State from, State to, Symbol pushed) {
    addPushRule(from, to, pushed, domain_.one());
  }
  std::size_t controls() const { return controls_; }
  const std::vector<Rule> &rules() const { return rules_; }
  const Domain &domain() const { return domain_; }
  PreparedWeight prepareWeight(const Weight &weight) const {
    return DomainPreparedWeight<Domain>::prepare(domain_, weight);
  }
  std::shared_ptr<const CompiledRuleIndexes> compiledRuleIndexes() const {
    return indexes_;
  }

private:
  CompiledRuleIndexes &writeIndexes() {
    if (indexes_.use_count() != 1)
      indexes_ = std::make_shared<CompiledRuleIndexes>(*indexes_);
    return *indexes_;
  }
  static std::size_t addGenerated(CompiledRuleIndexes &indexes, RuleKey key) {
    const std::size_t slot = indexes.generated_slots.size();
    auto inserted = indexes.generated_slots.emplace(key, slot);
    return inserted.first->second;
  }
  void indexRule(std::size_t index) {
    auto &rule = rules_[index];
    if (rule.weight == domain_.zero())
      return;
    auto &indexes = writeIndexes();
    if (rule.kind == RuleKind::Exact) {
      indexes.post.exact[{rule.from, rule.top}].push_back(index);
      if (rule.replacement.size() == 2)
        rule.generated_slot =
            addGenerated(indexes, {rule.to, rule.replacement[0]});
      if (rule.replacement.empty())
        indexes.pre.initial.push_back(index);
      else
        indexes.pre.exact[{rule.to, rule.replacement[0]}].push_back(index);
      return;
    }
    indexes.post.wildcard[rule.from].push_back(index);
    if (rule.kind == RuleKind::PreserveAny) {
      indexes.pre.wildcard[rule.to].push_back(index);
    } else {
      rule.generated_slot =
          addGenerated(indexes, {rule.to, rule.replacement[0]});
      indexes.pre.exact[{rule.to, rule.replacement[0]}].push_back(index);
    }
  }
  void addWildcardRule(RuleKind kind, State from, State to, Symbol pushed,
                       const Weight &weight) {
    if (from >= controls_ || to >= controls_)
      throw std::out_of_range("PDS control");
    (void)domain_.combine(domain_.zero(), weight);
    std::vector<Symbol> replacement;
    if (kind == RuleKind::PushAny)
      replacement.push_back(pushed);
    rules_.push_back({kind, from, 0, to, std::move(replacement), weight,
                      prepareWeight(weight)});
    indexRule(rules_.size() - 1);
  }
  Domain domain_;
  std::size_t controls_ = 0;
  std::vector<Rule> rules_;
  std::shared_ptr<CompiledRuleIndexes> indexes_;
};

template <class Domain> class SaturationSession;

template <class Domain, class = void>
struct HasExtendAndCombine : std::false_type {};
template <class Domain>
struct HasExtendAndCombine<
    Domain,
    std::void_t<decltype(std::declval<const Domain &>().extendAndCombine(
        std::declval<typename Domain::Weight &>(),
        std::declval<const typename Domain::Weight &>(),
        std::declval<const typename Domain::Weight &>()))>> : std::true_type {};

template <class Domain = BooleanSemiring> class Automaton {
public:
  using Weight = typename Domain::Weight;
  using TransitionId = std::size_t;
  struct TransitionRecord {
    Transition edge;
    Weight weight;
    bool queued = false;
    bool processed = false;
    bool force_full = false;
  };
  Automaton(const Automaton &) = delete;
  Automaton &operator=(const Automaton &) = delete;
  Automaton(Automaton &&) = default;
  Automaton &operator=(Automaton &&) = default;
  const std::vector<TransitionRecord> &transitions() const { return edges_; }
  const TransitionRecord *findTransition(const Transition &edge) const {
    if (edge.from >= states() || edge.to >= states())
      return nullptr;
    const TransitionId id = edge_ids_.find(edge, edges_);
    return id == TransitionIndex::Missing ? nullptr : &edges_[id];
  }
  const std::set<State> &finals() const { return finals_; }
  const Statistics &statistics() const { return stats_; }
  const Domain &domain() const { return domain_; }
  std::size_t controls() const { return controls_; }
  std::size_t states() const { return out_.size(); }
  Direction direction() const { return direction_; }

  Weight weight(State control, const std::vector<Symbol> &stack) const {
    const auto started = std::chrono::steady_clock::now();
    auto active = consume(control, stack);
    auto result = acceptingWeight(active);
    recordReadout(started);
    return result;
  }
  bool accepts(State control, const std::vector<Symbol> &stack) const {
    return weight(control, stack) != domain_.zero();
  }
  // Aggregate all accepted stacks beginning with prefix; {} means any stack.
  // This is NOT the same query as accepts(control,{}), which requires empty.
  Weight weightWithPrefix(State control,
                          const std::vector<Symbol> &prefix) const {
    const auto started = std::chrono::steady_clock::now();
    auto active = consume(control, prefix);
    close(active, false);
    auto result = acceptingWeight(active);
    recordReadout(started);
    return result;
  }
  bool acceptsPrefix(State control, const std::vector<Symbol> &prefix) const {
    return weightWithPrefix(control, prefix) != domain_.zero();
  }
  // Evaluate one word/prefix for every PDS control with a single reverse
  // weighted fixed point, rather than restarting a forward closure per control.
  std::vector<Weight> controlWeights(const std::vector<Symbol> &word) const {
    const auto started = std::chrono::steady_clock::now();
    ValuationVector suffix(states(), domain_.zero());
    for (State q : finals_)
      combineAt(suffix, q, domain_.one());
    reverseClose(suffix, true);
    for (auto symbol = word.rbegin(); symbol != word.rend(); ++symbol) {
      if (*symbol == Epsilon)
        throw std::invalid_argument("epsilon in query stack");
      suffix = reverseStep(suffix, *symbol);
      reverseClose(suffix, true);
    }
    suffix.resize(controls_);
    recordReadout(started);
    return suffix;
  }
  std::vector<Weight>
  controlWeightsWithPrefix(const std::vector<Symbol> &prefix) const {
    const auto started = std::chrono::steady_clock::now();
    ValuationVector suffix(states(), domain_.zero());
    for (State q : finals_)
      combineAt(suffix, q, domain_.one());
    reverseClose(suffix, false);
    for (auto symbol = prefix.rbegin(); symbol != prefix.rend(); ++symbol) {
      if (*symbol == Epsilon)
        throw std::invalid_argument("epsilon in query stack");
      suffix = reverseStep(suffix, *symbol);
      reverseClose(suffix, true);
    }
    suffix.resize(controls_);
    recordReadout(started);
    return suffix;
  }

private:
  friend class SaturationSession<Domain>;
  Automaton(Domain domain, std::size_t controls, Direction direction)
      : domain_(std::move(domain)), controls_(controls), direction_(direction) {
  }
  // post* automaton paths encode execution from right to left. pre* paths
  // encode execution from left to right. Never assume extend is commutative.
  Weight pathProduct(const Weight &left, const Weight &right) const {
    return direction_ == Direction::Post ? domain_.extend(right, left)
                                         : domain_.extend(left, right);
  }
  using Valuation = std::map<State, Weight>;
  using ValuationVector = std::vector<Weight>;
  bool combineAt(ValuationVector &values, State state,
                 const Weight &candidate) const {
    if constexpr (std::is_same_v<Weight, bool>) {
      Weight value = values[state];
      const bool changed = domain_.combineWith(value, candidate);
      values[state] = value;
      return changed;
    } else {
      return domain_.combineWith(values[state], candidate);
    }
  }
  Weight prepend(const Weight &edge, const Weight &suffix) const {
    return direction_ == Direction::Post ? domain_.extend(suffix, edge)
                                         : domain_.extend(edge, suffix);
  }
  void recordReadout(std::chrono::steady_clock::time_point started) const {
    stats_.readout_microseconds += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
  }
  void reverseClose(ValuationVector &values, bool epsilon_only) const {
    std::deque<State> queue;
    std::vector<bool> pending(states(), false);
    for (State q = 0; q < states(); ++q)
      if (values[q] != domain_.zero()) {
        queue.push_back(q);
        pending[q] = true;
      }
    while (!queue.empty()) {
      const State to = queue.front();
      queue.pop_front();
      pending[to] = false;
      const Weight suffix = values[to];
      const auto &incoming = epsilon_only ? epsilon_in_[to] : in_[to];
      for (TransitionId id : incoming) {
        const auto &entry = edges_[id];
        const auto &edge = entry.edge;
        const Weight candidate = prepend(entry.weight, suffix);
        if (candidate == domain_.zero())
          continue;
        if (combineAt(values, edge.from, candidate)) {
          if (!pending[edge.from]) {
            pending[edge.from] = true;
            queue.push_back(edge.from);
          }
        }
      }
    }
  }
  ValuationVector reverseStep(const ValuationVector &suffix,
                              Symbol label) const {
    ValuationVector result(states(), domain_.zero());
    for (State to = 0; to < states(); ++to) {
      if (suffix[to] == domain_.zero())
        continue;
      for (TransitionId id : in_[to]) {
        const auto &entry = edges_[id];
        const auto &edge = entry.edge;
        if (edge.label == label) {
          const Weight candidate = prepend(entry.weight, suffix[to]);
          combineAt(result, edge.from, candidate);
        }
      }
    }
    return result;
  }
  void close(Valuation &active, bool epsilon_only) const {
    std::deque<State> queue;
    std::set<State> pending;
    for (const auto &entry : active) {
      queue.push_back(entry.first);
      pending.insert(entry.first);
    }
    while (!queue.empty()) {
      State from = queue.front();
      queue.pop_front();
      pending.erase(from);
      const Weight source = active.at(from);
      const auto &outgoing = epsilon_only ? epsilon_out_[from] : out_[from];
      for (TransitionId id : outgoing) {
        const auto &entry = edges_[id];
        const auto &edge = entry.edge;
        Weight candidate = pathProduct(source, entry.weight);
        if (candidate == domain_.zero())
          continue;
        auto it = active.find(edge.to);
        const bool changed = it == active.end()
                                 ? active.emplace(edge.to, candidate).second
                                 : domain_.combineWith(it->second, candidate);
        if (changed) {
          if (pending.insert(edge.to).second)
            queue.push_back(edge.to);
        }
      }
    }
  }
  Valuation consume(State control, const std::vector<Symbol> &word) const {
    if (control >= controls_)
      throw std::out_of_range("query control");
    Valuation active;
    active.emplace(control, domain_.one());
    close(active, true);
    for (Symbol a : word) {
      if (a == Epsilon)
        throw std::invalid_argument("epsilon in query stack");
      Valuation next;
      for (const auto &entry : active)
        for (TransitionId id : out_[entry.first]) {
          const auto &adjacent = edges_[id];
          const auto &edge = adjacent.edge;
          if (edge.label == a) {
            const Weight candidate = pathProduct(entry.second, adjacent.weight);
            if (candidate == domain_.zero())
              continue;
            auto it = next.find(edge.to);
            if (it == next.end())
              next.emplace(edge.to, candidate);
            else
              domain_.combineWith(it->second, candidate);
          }
        }
      active = std::move(next);
      close(active, true);
    }
    return active;
  }
  Weight acceptingWeight(const Valuation &active) const {
    Weight result = domain_.zero();
    for (State q : finals_) {
      auto it = active.find(q);
      if (it != active.end())
        domain_.combineWith(result, it->second);
    }
    return result;
  }
  Domain domain_;
  std::size_t controls_;
  Direction direction_;
  std::vector<TransitionRecord> edges_;
  TransitionIndex edge_ids_;
  std::vector<std::vector<TransitionId>> out_, in_;
  std::vector<std::vector<TransitionId>> epsilon_out_, epsilon_in_;
  std::set<State> finals_;
  mutable Statistics stats_;
};

// Incremental weighted post*/pre* saturation. Controls are fixed; clients can
// monotonically add rules (e.g. alias-induced flows) and resume run(). Every
// improved weight is rescheduled, not just every newly discovered transition.
// Domain must be a finite-height idempotent semiring; zero must annihilate
// extend.
template <class Domain = BooleanSemiring> class SaturationSession {
public:
  using System = PushdownSystem<Domain>;
  using Weight = typename Domain::Weight;
  using TransitionId = typename Automaton<Domain>::TransitionId;
  using Delta = typename DomainDelta<Domain>::Type;
  static constexpr bool HAS_DELTA = DomainDelta<Domain>::available;
  SaturationSession(const System &system, const RegularSet &seed,
                    Direction direction = Direction::Post, Limits limits = {})
      : system_(system), base_indexes_(system.compiledRuleIndexes()),
        result_(system.domain(), system.controls(), direction),
        base_rule_count_(system.rules().size()), limits_(limits),
        wildcard_indexed_(system.controls()) {
    if (seed.controls() != system.controls())
      throw std::invalid_argument("seed/PDS control count mismatch");
    const auto setup_started = std::chrono::steady_clock::now();
    // Clone only seed states that can reach a final. Dead seed states cannot
    // affect the accepted configuration language, and cloning every inactive
    // control makes a singleton query start with roughly twice as many states.
    std::vector<std::vector<State>> seed_predecessors(seed.states());
    for (const auto &edge : seed.transitions())
      seed_predecessors[edge.to].push_back(edge.from);
    std::vector<bool> live(seed.states(), false);
    std::deque<State> live_queue;
    for (State final : seed.finals()) {
      if (!live[final]) {
        live[final] = true;
        live_queue.push_back(final);
      }
    }
    while (!live_queue.empty()) {
      const State state = live_queue.front();
      live_queue.pop_front();
      for (State predecessor : seed_predecessors[state])
        if (!live[predecessor]) {
          live[predecessor] = true;
          live_queue.push_back(predecessor);
        }
    }
    for (State p = 0; p < system.controls(); ++p)
      addState();
    const State missing = std::numeric_limits<State>::max();
    std::vector<State> clone(seed.states(), missing);
    for (State q = 0; q < seed.states(); ++q)
      if (live[q])
        clone[q] = addState();
    for (State q : seed.finals())
      result_.finals_.insert(clone[q]);
    for (const auto &e : seed.transitions())
      if (live[e.from] && live[e.to])
        relax({clone[e.from], e.label, clone[e.to]}, domain().one());
    for (State p = 0; p < system.controls(); ++p)
      if (clone[p] != missing)
        relax({p, Epsilon, clone[p]}, domain().one());
    const auto &base_index =
        direction == Direction::Post ? base_indexes_->post
                                     : base_indexes_->pre;
    if (direction == Direction::Post)
      for (std::size_t i = 0; i < base_indexes_->generated_slots.size(); ++i)
        base_generated_.push_back(addState());
    for (std::size_t i : base_index.initial) {
      const auto &r = rule(i);
      relax({r.from, r.top, r.to}, r.weight);
    }
    result_.stats_.rules = base_rule_count_;
    result_.stats_.setup_microseconds += microsecondsSince(setup_started);
  }
  void addRule(State from, Symbol top, State to,
               std::vector<Symbol> replacement, const Weight &weight) {
    checkUsable();
    complete_ = false;
    try {
      if (from >= result_.controls() || to >= result_.controls())
        throw std::out_of_range("PDS control");
      if (top == Epsilon || replacement.size() > 2 ||
          std::find(replacement.begin(), replacement.end(), Epsilon) !=
              replacement.end())
        throw std::invalid_argument(
            "PDS rule must replace one symbol by at most two");
      (void)domain().combine(domain().zero(), weight);
      added_rules_.push_back({System::RuleKind::Exact, from, top, to,
                              std::move(replacement), weight,
                              system_.prepareWeight(weight)});
      installRule(base_rule_count_ + added_rules_.size() - 1);
    } catch (...) {
      poisoned_ = true;
      throw;
    }
  }
  void addRule(State from, Symbol top, State to,
               std::vector<Symbol> replacement) {
    addRule(from, top, to, std::move(replacement), domain().one());
  }
  const Automaton<Domain> &run() {
    checkUsable();
    complete_ = false;
    const auto saturation_started = std::chrono::steady_clock::now();
    try {
      while (!queue_.empty()) {
        const auto pending = queue_.front();
        queue_.pop_front();
        auto &record = result_.edges_[pending];
        const Transition edge = record.edge;
        record.queued = false;
        if constexpr (HAS_DELTA) {
          const bool first = !record.processed;
          const bool forced = record.force_full;
          const bool full = first || forced;
          record.processed = true;
          record.force_full = false;
          std::optional<Delta> delta;
          if (!full)
            takePendingDelta(pending, delta);
          else if (forced)
            discardPendingDelta(pending);
          ++result_.stats_.processed;
          const Weight value = record.weight;
          if (full || !delta) {
            propagateEpsilon(edge, value);
            if (edge.label != Epsilon) {
              if (result_.direction_ == Direction::Post)
                propagatePost(edge, value);
              else
                propagatePre(pending, edge, value);
            }
          } else {
            propagateEpsilonDelta(pending, edge, value, &*delta);
            if (edge.label != Epsilon) {
              if (result_.direction_ == Direction::Post)
                propagatePostDelta(edge, value, &*delta);
              else
                propagatePreDelta(pending, edge, value, &*delta);
            }
          }
        } else {
          ++result_.stats_.processed;
          const Weight value = record.weight;
          propagateEpsilon(edge, value);
          if (edge.label != Epsilon) {
            if (result_.direction_ == Direction::Post)
              propagatePost(edge, value);
            else
              propagatePre(pending, edge, value);
          }
        }
      }
      result_.stats_.saturation_microseconds +=
          microsecondsSince(saturation_started);
      complete_ = true;
      return result_;
    } catch (...) {
      poisoned_ = true;
      throw;
    }
  }
  const Automaton<Domain> &result() const {
    checkUsable();
    if (!complete_)
      throw std::logic_error("SPDS saturation is not complete");
    return result_;
  }
  Automaton<Domain> takeResult() {
    checkUsable();
    if (!complete_)
      throw std::logic_error("SPDS saturation is not complete");
    return std::move(result_);
  }

private:
  static std::uint64_t
  microsecondsSince(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
  }
  using Key = RuleKey;
  using KeyHash = RuleKeyHash;
  struct Waiting {
    std::size_t rule;
    TransitionId first;
    bool operator==(const Waiting &other) const {
      return rule == other.rule && first == other.first;
    }
  };
  struct WaitingHash {
    std::size_t operator()(const Waiting &waiting) const {
      std::size_t seed = std::hash<std::size_t>{}(waiting.rule);
      seed ^= std::hash<TransitionId>{}(waiting.first) + 0x9e3779b9U +
              (seed << 6U) + (seed >> 2U);
      return seed;
    }
  };
  using DeltaSlot = std::uint32_t;
  static constexpr std::size_t DELTA_PAGE_SIZE = 4096;
  static constexpr DeltaSlot NO_DELTA_SLOT =
      std::numeric_limits<DeltaSlot>::max();
  struct DeltaPage {
    DeltaPage() { slots.fill(NO_DELTA_SLOT); }
    std::array<DeltaSlot, DELTA_PAGE_SIZE> slots;
  };
  const typename System::Rule &rule(std::size_t i) const {
    if (i < base_rule_count_)
      return system_.rules()[i];
    return added_rules_.at(i - base_rule_count_);
  }
  std::size_t ruleCount() const {
    return base_rule_count_ + added_rules_.size();
  }
  const Domain &domain() const { return system_.domain(); }
  void checkUsable() const {
    if (poisoned_)
      throw std::logic_error("failed SPDS session; discard and restart");
  }
  State addState() {
    if (limits_.max_states && result_.states() >= limits_.max_states)
      throw ResourceLimit("SPDS state limit exceeded");
    State next = result_.states();
    result_.out_.emplace_back();
    result_.in_.emplace_back();
    result_.epsilon_out_.emplace_back();
    result_.epsilon_in_.emplace_back();
    result_.stats_.states = result_.states();
    return next;
  }
  void schedule(TransitionId id, bool force_full = false) {
    auto &record = result_.edges_[id];
    if constexpr (HAS_DELTA)
      record.force_full = record.force_full || force_full;
    if (!record.queued) {
      record.queued = true;
      queue_.push_back(id);
    }
  }
  DeltaPage *deltaPage(TransitionId id, bool create) {
    const std::size_t page_index = id / DELTA_PAGE_SIZE;
    if (page_index >= pending_delta_pages_.size()) {
      if (!create)
        return nullptr;
      pending_delta_pages_.resize(page_index + 1);
    }
    auto &page = pending_delta_pages_[page_index];
    if (!page && create)
      page = std::make_unique<DeltaPage>();
    return page.get();
  }
  void mergePendingDelta(TransitionId id, Delta delta) {
    DeltaPage *page = deltaPage(id, true);
    DeltaSlot &slot = page->slots[id % DELTA_PAGE_SIZE];
    if (slot == NO_DELTA_SLOT) {
      if (!free_delta_slots_.empty()) {
        slot = free_delta_slots_.back();
        free_delta_slots_.pop_back();
        delta_arena_[slot] = std::move(delta);
      } else {
        if (delta_arena_.size() >= NO_DELTA_SLOT)
          throw ResourceLimit("SPDS pending-delta arena limit exceeded");
        slot = static_cast<DeltaSlot>(delta_arena_.size());
        delta_arena_.push_back(std::move(delta));
      }
      return;
    }
    domain().mergeDelta(delta_arena_[slot], std::move(delta));
  }
  void releasePendingDelta(DeltaSlot &slot) {
    if (slot == NO_DELTA_SLOT)
      return;
    delta_arena_[slot] = Delta{};
    free_delta_slots_.push_back(slot);
    slot = NO_DELTA_SLOT;
  }
  void takePendingDelta(TransitionId id, std::optional<Delta> &output) {
    DeltaPage *page = deltaPage(id, false);
    if (!page)
      return;
    DeltaSlot &slot = page->slots[id % DELTA_PAGE_SIZE];
    if (slot == NO_DELTA_SLOT)
      return;
    output.emplace(std::move(delta_arena_[slot]));
    releasePendingDelta(slot);
  }
  void discardPendingDelta(TransitionId id) {
    DeltaPage *page = deltaPage(id, false);
    if (page)
      releasePendingDelta(page->slots[id % DELTA_PAGE_SIZE]);
  }
  void relax(const Transition &edge, const Weight &candidate) {
    relaxImpl(edge, candidate);
  }
  void relax(const Transition &edge, Weight &&candidate) {
    relaxImpl(edge, std::move(candidate));
  }
  template <class Update>
  bool updateExistingFull(TransitionId id, Update &&update) {
    auto &record = result_.edges_[id];
    if (limits_.max_updates) {
      Weight joined = record.weight;
      if (!update(joined))
        return false;
      if (result_.stats_.updates >= limits_.max_updates)
        throw ResourceLimit("SPDS weight-update limit exceeded");
      record.weight = std::move(joined);
    } else if (!update(record.weight)) {
      return false;
    }
    return true;
  }
  template <class Update>
  bool updateExistingDelta(TransitionId id, Update &&update) {
    auto &record = result_.edges_[id];
    if (!record.processed) {
      if (limits_.max_updates) {
        Weight joined = record.weight;
        if (!update(joined, nullptr))
          return false;
        if (result_.stats_.updates >= limits_.max_updates)
          throw ResourceLimit("SPDS weight-update limit exceeded");
        record.weight = std::move(joined);
      } else if (!update(record.weight, nullptr)) {
        return false;
      }
      return true;
    }
    Delta produced;
    if (limits_.max_updates) {
      Weight joined = record.weight;
      if (!update(joined, &produced))
        return false;
      if (result_.stats_.updates >= limits_.max_updates)
        throw ResourceLimit("SPDS weight-update limit exceeded");
      record.weight = std::move(joined);
    } else if (!update(record.weight, &produced)) {
      return false;
    }
    if (!produced.empty()) {
      mergePendingDelta(id, std::move(produced));
    }
    return true;
  }
  template <class Candidate>
  void insertNew(const Transition &edge, Candidate &&candidate) {
    if (candidate == domain().zero())
      return;
    if (limits_.max_transitions &&
        result_.edges_.size() >= limits_.max_transitions)
      throw ResourceLimit("SPDS transition limit exceeded");
    if (limits_.max_updates && result_.stats_.updates >= limits_.max_updates)
      throw ResourceLimit("SPDS weight-update limit exceeded");
    const auto id = result_.edges_.size();
    result_.edges_.push_back({edge, std::forward<Candidate>(candidate)});
    try {
      result_.edge_ids_.insert(edge, id, result_.edges_);
    } catch (...) {
      result_.edges_.pop_back();
      throw;
    }
    result_.out_[edge.from].push_back(id);
    result_.in_[edge.to].push_back(id);
    if (edge.label == Epsilon) {
      result_.epsilon_out_[edge.from].push_back(id);
      result_.epsilon_in_[edge.to].push_back(id);
    }
    ++result_.stats_.updates;
    result_.stats_.transitions = result_.edges_.size();
    schedule(id);
  }
  template <class Candidate>
  void relaxImpl(const Transition &edge, Candidate &&candidate) {
    if (candidate == domain().zero())
      return;
    const TransitionId id = result_.edge_ids_.find(edge, result_.edges_);
    if (id == TransitionIndex::Missing) {
      insertNew(edge, std::forward<Candidate>(candidate));
      return;
    }
    bool changed;
    if constexpr (HAS_DELTA)
      changed = updateExistingDelta(id, [&](Weight &weight, Delta *delta) {
        return delta ? domain().combineWithDelta(weight, candidate, delta)
                     : domain().combineWith(weight, candidate);
      });
    else
      changed = updateExistingFull(id, [&](Weight &weight) {
        return domain().combineWith(weight, candidate);
      });
    if (!changed)
      return;
    ++result_.stats_.updates;
    result_.stats_.transitions = result_.edges_.size();
    schedule(id);
  }
  void relaxExtended(const Transition &edge, const Weight &left,
                     const Weight &right) {
    if (left == domain().zero() || right == domain().zero())
      return;
    const TransitionId id = result_.edge_ids_.find(edge, result_.edges_);
    if (id == TransitionIndex::Missing) {
      insertNew(edge, domain().extend(left, right));
      return;
    }
    bool changed;
    if constexpr (HasExtendAndCombine<Domain>::value) {
      if constexpr (HAS_DELTA)
        changed =
            updateExistingDelta(id, [&](Weight &weight, Delta *delta) {
              return delta ? domain().extendAndCombineDelta(
                                 weight, left, right, delta)
                           : domain().extendAndCombine(weight, left, right);
            });
      else
        changed = updateExistingFull(id, [&](Weight &weight) {
          return domain().extendAndCombine(weight, left, right);
        });
    } else {
      Weight candidate = domain().extend(left, right);
      if constexpr (HAS_DELTA)
        changed =
            updateExistingDelta(id, [&](Weight &weight, Delta *delta) {
              return delta
                         ? domain().combineWithDelta(weight, candidate, delta)
                         : domain().combineWith(weight, candidate);
            });
      else
        changed = updateExistingFull(id, [&](Weight &weight) {
          return domain().combineWith(weight, candidate);
        });
    }
    if (!changed)
      return;
    ++result_.stats_.updates;
    result_.stats_.transitions = result_.edges_.size();
    schedule(id);
  }
  void relaxRuleExtended(const Transition &edge, const Weight &left,
                         const typename System::Rule &rule) {
    if constexpr (!System::HAS_PREPARED_WEIGHT) {
      relaxExtended(edge, left, rule.weight);
      return;
    } else {
      if (left == domain().zero() || rule.weight == domain().zero())
        return;
      const TransitionId id = result_.edge_ids_.find(edge, result_.edges_);
      if (id == TransitionIndex::Missing) {
        insertNew(edge,
                  domain().extendPrepared(left, rule.weight, rule.prepared));
        return;
      }
      bool changed;
      if constexpr (HAS_DELTA)
        changed =
            updateExistingDelta(id, [&](Weight &weight, Delta *delta) {
              return delta ? domain().extendAndCombinePreparedDelta(
                                 weight, left, rule.weight, rule.prepared,
                                 delta)
                           : domain().extendAndCombinePrepared(
                                 weight, left, rule.weight, rule.prepared);
            });
      else
        changed = updateExistingFull(id, [&](Weight &weight) {
          return domain().extendAndCombinePrepared(
              weight, left, rule.weight, rule.prepared);
        });
      if (!changed)
        return;
      ++result_.stats_.updates;
      result_.stats_.transitions = result_.edges_.size();
      schedule(id);
    }
  }
  void relaxPath(const Transition &edge, const Weight &left,
                 const Weight &right) {
    if (result_.direction_ == Direction::Post)
      relaxExtended(edge, right, left);
    else
      relaxExtended(edge, left, right);
  }
  void relaxDeltaExtended(const Transition &edge, const Weight &left,
                          const Weight &right, const Delta &input,
                          bool input_is_left) {
    if constexpr (!HAS_DELTA) {
      relaxExtended(edge, left, right);
    } else {
      if (input.empty())
        return;
      const TransitionId id = result_.edge_ids_.find(edge, result_.edges_);
      if (id == TransitionIndex::Missing) {
        relaxExtended(edge, left, right);
        return;
      }
      if (!updateExistingDelta(id, [&](Weight &weight, Delta *output) {
            return domain().extendDeltaAndCombine(
                weight, left, right, input, input_is_left, output);
          }))
        return;
      ++result_.stats_.updates;
      schedule(id);
    }
  }
  void relaxRuleDelta(const Transition &edge, const Weight &left,
                      const typename System::Rule &rule,
                      const Delta &input) {
    if constexpr (!HAS_DELTA) {
      relaxRuleExtended(edge, left, rule);
    } else if constexpr (System::HAS_PREPARED_WEIGHT) {
      const TransitionId id = result_.edge_ids_.find(edge, result_.edges_);
      if (id == TransitionIndex::Missing) {
        relaxRuleExtended(edge, left, rule);
        return;
      }
      if (!updateExistingDelta(id, [&](Weight &weight, Delta *output) {
            return domain().extendPreparedInputDeltaAndCombine(
                weight, left, rule.weight, rule.prepared, input, output);
          }))
        return;
      ++result_.stats_.updates;
      schedule(id);
    } else {
      relaxDeltaExtended(edge, left, rule.weight, input, true);
    }
  }
  void installRule(std::size_t i, bool schedule_existing = true) {
    const auto &r = rule(i);
    result_.stats_.rules = ruleCount();
    if (r.weight == domain().zero())
      return;
    Key key;
    if (result_.direction_ == Direction::Post) {
      if (r.kind != System::RuleKind::Exact) {
        wildcard_indexed_[r.from].push_back(i);
        if (r.kind == System::RuleKind::PushAny) {
          Key generated{r.to, r.replacement[0]};
          if (!base_indexes_->generated_slots.count(generated) &&
              !generated_.count(generated))
            generated_.emplace(generated, addState());
        }
        if (schedule_existing)
          for (const auto id : result_.out_[r.from])
            if (result_.edges_[id].edge.label != Epsilon)
              schedule(id, true);
        return;
      }
      key = {r.from, r.top};
      if (r.replacement.size() == 2) {
        Key generated{r.to, r.replacement[0]};
        if (!base_indexes_->generated_slots.count(generated) &&
            !generated_.count(generated))
          generated_.emplace(generated, addState());
      }
    } else {
      if (r.kind == System::RuleKind::PreserveAny) {
        wildcard_indexed_[r.to].push_back(i);
        if (schedule_existing)
          for (const auto id : result_.out_[r.to])
            if (result_.edges_[id].edge.label != Epsilon)
              schedule(id, true);
        return;
      }
      if (r.kind == System::RuleKind::PushAny) {
        key = {r.to, r.replacement[0]};
        indexed_[key].push_back(i);
        if (schedule_existing)
          for (const auto id : result_.out_[key.first])
            if (result_.edges_[id].edge.label == key.second)
              schedule(id, true);
        return;
      }
      if (r.replacement.empty()) {
        relax({r.from, r.top, r.to}, r.weight);
        return;
      }
      key = {r.to, r.replacement[0]};
    }
    indexed_[key].push_back(i);
    if (schedule_existing)
      for (const auto id : result_.out_[key.first])
        if (result_.edges_[id].edge.label == key.second)
          schedule(id, true);
  }
  template <class Function>
  void forEachIndexedRule(const Key &key, Function &&function) {
    const auto &base = (result_.direction_ == Direction::Post
                            ? base_indexes_->post
                            : base_indexes_->pre)
                           .exact;
    auto found = base.find(key);
    if (found != base.end())
      for (std::size_t index : found->second)
        function(index);
    auto added = indexed_.find(key);
    if (added != indexed_.end())
      for (std::size_t index : added->second)
        function(index);
  }
  State generatedState(std::size_t rule_index,
                       const typename System::Rule &rule) const {
    if (rule_index < base_rule_count_)
      return base_generated_.at(rule.generated_slot);
    const Key key{rule.to, rule.replacement[0]};
    auto base = base_indexes_->generated_slots.find(key);
    return base != base_indexes_->generated_slots.end()
               ? base_generated_.at(base->second)
               : generated_.at(key);
  }
  template <class Function>
  void forEachWildcardRule(State control, Function &&function) {
    const auto &base = (result_.direction_ == Direction::Post
                            ? base_indexes_->post
                            : base_indexes_->pre)
                           .wildcard;
    if (control < base.size())
      for (std::size_t index : base[control])
        function(index);
    if (control < wildcard_indexed_.size())
      for (std::size_t index : wildcard_indexed_[control])
        function(index);
  }
  void registerWaiting(const Key &key, const Waiting &waiting) {
    if (registered_waiting_.insert(waiting).second)
      waiting_[key].push_back(waiting);
  }
  void propagateEpsilon(const Transition &e, const Weight &value) {
    const auto &incoming =
        e.label == Epsilon ? result_.in_[e.from] : result_.epsilon_in_[e.from];
    const std::size_t incoming_size = incoming.size();
    for (std::size_t i = 0; i < incoming_size; ++i) {
      const auto &record = result_.edges_[incoming[i]];
      const auto &left = record.edge;
      relaxPath({left.from,
                 left.label == Epsilon ? e.label : left.label, e.to},
                record.weight, value);
    }
    const auto &outgoing =
        e.label == Epsilon ? result_.out_[e.to] : result_.epsilon_out_[e.to];
    const std::size_t outgoing_size = outgoing.size();
    for (std::size_t i = 0; i < outgoing_size; ++i) {
      const auto &record = result_.edges_[outgoing[i]];
      const auto &right = record.edge;
      relaxPath({e.from,
                 e.label == Epsilon ? right.label : e.label, right.to},
                value, record.weight);
    }
  }
  void propagatePost(const Transition &e, const Weight &value) {
    forEachIndexedRule({e.from, e.label}, [&](std::size_t i) {
      const auto &r = rule(i);
      if (r.replacement.empty())
        relaxRuleExtended({r.to, Epsilon, e.to}, value, r);
      else if (r.replacement.size() == 1)
        relaxRuleExtended({r.to, r.replacement[0], e.to}, value, r);
      else {
        State mid = generatedState(i, r);
        relax({r.to, r.replacement[0], mid}, domain().one());
        relaxRuleExtended({mid, r.replacement[1], e.to}, value, r);
      }
    });
    forEachWildcardRule(e.from, [&](std::size_t i) {
      const auto &r = rule(i);
      if (r.kind == System::RuleKind::PreserveAny) {
        relaxRuleExtended({r.to, e.label, e.to}, value, r);
      } else {
        State mid = generatedState(i, r);
        relax({r.to, r.replacement[0], mid}, domain().one());
        relaxRuleExtended({mid, e.label, e.to}, value, r);
      }
    });
  }
  void propagatePre(TransitionId edge_id, const Transition &e,
                    const Weight &value) {
    forEachIndexedRule({e.from, e.label}, [&](std::size_t i) {
      const auto &r = rule(i);
      if (r.kind == System::RuleKind::PushAny) {
        Waiting waiting{i, edge_id};
        registerWaiting({e.to, Epsilon}, waiting);
        const std::size_t count = result_.out_[e.to].size();
        for (std::size_t j = 0; j < count; ++j) {
          const TransitionId second_id = result_.out_[e.to][j];
          if (result_.edges_[second_id].edge.label != Epsilon)
            joinPush(waiting, second_id);
        }
      } else if (r.replacement.size() == 1) {
        relaxExtended({r.from, r.top, e.to}, r.weight, value);
      } else {
        Waiting waiting{i, edge_id};
        registerWaiting({e.to, r.replacement[1]}, waiting);
        const std::size_t count = result_.out_[e.to].size();
        for (std::size_t j = 0; j < count; ++j) {
          const TransitionId second_id = result_.out_[e.to][j];
          if (result_.edges_[second_id].edge.label == r.replacement[1])
            joinPush(waiting, second_id);
        }
      }
    });
    forEachWildcardRule(e.from, [&](std::size_t i) {
      const auto &r = rule(i);
      relaxExtended({r.from, e.label, e.to}, r.weight, value);
    });
    auto right = waiting_.find({e.from, e.label});
    if (right != waiting_.end())
      for (const auto &waiting : right->second)
        joinPush(waiting, edge_id);
    auto wildcard_right = waiting_.find({e.from, Epsilon});
    if (wildcard_right != waiting_.end())
      for (const auto &waiting : wildcard_right->second)
        joinPush(waiting, edge_id);
  }
  bool useEpsilonDelta(TransitionId other, const Delta *delta) const {
    // If both edges have been processed, the later first-processing step saw
    // the earlier edge and propagated the full pair. An unprocessed neighbor
    // is new, so use the full product once and let its own queued visit cover
    // subsequent deltas. This avoids storing every epsilon-composition pair.
    return HAS_DELTA && delta && result_.edges_[other].processed;
  }
  void propagateEpsilonDelta(TransitionId edge_id, const Transition &e,
                             const Weight &value, const Delta *delta) {
    // Capture the old size and reload by index: relax() may reallocate an
    // adjacency vector, but newly appended edges are scheduled separately.
    const auto &incoming =
        e.label == Epsilon ? result_.in_[e.from] : result_.epsilon_in_[e.from];
    const std::size_t incoming_size = incoming.size();
    for (std::size_t i = 0; i < incoming_size; ++i) {
      const TransitionId left_id = incoming[i];
      const auto &record = result_.edges_[left_id];
      const auto &left = record.edge;
      const Transition composed{
          left.from, left.label == Epsilon ? e.label : left.label, e.to};
      if constexpr (HAS_DELTA) {
        if (useEpsilonDelta(left_id, delta))
          relaxDeltaExtended(composed,
                             result_.direction_ == Direction::Post
                                 ? value
                                 : record.weight,
                             result_.direction_ == Direction::Post
                                 ? record.weight
                                 : value,
                             *delta,
                             result_.direction_ == Direction::Post);
        else
          relaxPath(composed, record.weight, value);
      } else {
        relaxPath(composed, record.weight, value);
      }
    }
    const auto &outgoing =
        e.label == Epsilon ? result_.out_[e.to] : result_.epsilon_out_[e.to];
    const std::size_t outgoing_size = outgoing.size();
    for (std::size_t i = 0; i < outgoing_size; ++i) {
      const TransitionId right_id = outgoing[i];
      const auto &record = result_.edges_[right_id];
      const auto &right = record.edge;
      const Transition composed{
          e.from, e.label == Epsilon ? right.label : e.label, right.to};
      if constexpr (HAS_DELTA) {
        if (useEpsilonDelta(right_id, delta))
          relaxDeltaExtended(composed,
                             result_.direction_ == Direction::Post
                                 ? record.weight
                                 : value,
                             result_.direction_ == Direction::Post
                                 ? value
                                 : record.weight,
                             *delta,
                             result_.direction_ != Direction::Post);
        else
          relaxPath(composed, value, record.weight);
      } else {
        relaxPath(composed, value, record.weight);
      }
    }
  }
  void propagatePostDelta(const Transition &e, const Weight &value,
                          const Delta *delta) {
    forEachIndexedRule({e.from, e.label}, [&](std::size_t i) {
      const auto &r = rule(i);
      // Base rules predate every transition. Incremental rules force a full
      // visit of all matching existing transitions, so a delta visit may
      // safely propagate only its new directions.
      const Delta *incremental = delta;
      if (r.replacement.empty())
        incremental ? relaxRuleDelta({r.to, Epsilon, e.to}, value, r,
                                     *incremental)
              : relaxRuleExtended({r.to, Epsilon, e.to}, value, r);
      else if (r.replacement.size() == 1)
        incremental ? relaxRuleDelta({r.to, r.replacement[0], e.to}, value, r,
                                     *incremental)
              : relaxRuleExtended({r.to, r.replacement[0], e.to}, value, r);
      else {
        State mid = generatedState(i, r);
        // The shared first edge carries ONE. Prior history and rule weight
        // belong on the continuation edge; mixing them loses correlations.
        relax({r.to, r.replacement[0], mid}, domain().one());
        if (incremental)
          relaxRuleDelta({mid, r.replacement[1], e.to}, value, r,
                         *incremental);
        else
          relaxRuleExtended({mid, r.replacement[1], e.to}, value, r);
      }
    });
    forEachWildcardRule(e.from, [&](std::size_t i) {
      const auto &r = rule(i);
      const Delta *incremental = delta;
      if (r.kind == System::RuleKind::PreserveAny) {
        if (incremental)
          relaxRuleDelta({r.to, e.label, e.to}, value, r, *incremental);
        else
          relaxRuleExtended({r.to, e.label, e.to}, value, r);
      } else {
        State mid = generatedState(i, r);
        relax({r.to, r.replacement[0], mid}, domain().one());
        if (incremental)
          relaxRuleDelta({mid, e.label, e.to}, value, r, *incremental);
        else
          relaxRuleExtended({mid, e.label, e.to}, value, r);
      }
    });
  }
  void joinPush(const Waiting &waiting, TransitionId second_id) {
    const auto &r = rule(waiting.rule);
    const Transition second = result_.edges_[second_id].edge;
    const Symbol top =
        r.kind == System::RuleKind::PushAny ? second.label : r.top;
    Weight continuation =
        domain().extend(result_.edges_[waiting.first].weight,
                        result_.edges_[second_id].weight);
    relaxExtended({r.from, top, second.to}, r.weight, continuation);
  }
  void joinPushDelta(const Waiting &waiting, TransitionId second_id,
                     const Delta &input, bool input_is_first) {
    if (input.empty())
      return;
    const auto &r = rule(waiting.rule);
    const Transition second = result_.edges_[second_id].edge;
    const Symbol top =
        r.kind == System::RuleKind::PushAny ? second.label : r.top;
    const Transition output_edge{r.from, top, second.to};
    const TransitionId output_id =
        result_.edge_ids_.find(output_edge, result_.edges_);
    if (output_id == TransitionIndex::Missing) {
      joinPush(waiting, second_id);
      return;
    }
    // The output transition may alias either operand in a cyclic automaton.
    // Cheap COW copies keep the full operands stable while the target grows.
    const Weight first = result_.edges_[waiting.first].weight;
    const Weight second_weight = result_.edges_[second_id].weight;
    if (!updateExistingDelta(
            output_id, [&](Weight &weight, Delta *output) {
              return domain().extendPushDeltaAndCombine(
                  weight, r.weight, first, second_weight, input,
                  input_is_first, output);
            }))
      return;
    ++result_.stats_.updates;
    schedule(output_id);
  }
  void propagatePreDelta(TransitionId edge_id, const Transition &e,
                         const Weight &value, const Delta *delta) {
    forEachIndexedRule({e.from, e.label}, [&](std::size_t i) {
      const auto &r = rule(i);
      if (r.kind == System::RuleKind::PushAny) {
        Waiting waiting{i, edge_id};
        registerWaiting({e.to, Epsilon}, waiting);
        const std::size_t count = result_.out_[e.to].size();
        for (std::size_t j = 0; j < count; ++j) {
          const TransitionId second_id = result_.out_[e.to][j];
          if (result_.edges_[second_id].edge.label != Epsilon)
            result_.edges_[second_id].processed
                ? joinPushDelta(waiting, second_id, *delta, true)
                : joinPush(waiting, second_id);
        }
      } else if (r.replacement.size() == 1) {
        const Delta *incremental = delta;
        if (incremental)
          relaxDeltaExtended({r.from, r.top, e.to}, r.weight, value,
                             *incremental, false);
        else
          relaxExtended({r.from, r.top, e.to}, r.weight, value);
      } else {
        Waiting waiting{i, edge_id};
        registerWaiting({e.to, r.replacement[1]}, waiting);
        const std::size_t count = result_.out_[e.to].size();
        for (std::size_t j = 0; j < count; ++j) {
          const TransitionId second_id = result_.out_[e.to][j];
          if (result_.edges_[second_id].edge.label == r.replacement[1])
            result_.edges_[second_id].processed
                ? joinPushDelta(waiting, second_id, *delta, true)
                : joinPush(waiting, second_id);
        }
      }
    });
    forEachWildcardRule(e.from, [&](std::size_t i) {
      const auto &r = rule(i);
      const Delta *incremental = delta;
      if (incremental)
        relaxDeltaExtended({r.from, e.label, e.to}, r.weight, value,
                           *incremental, false);
      else
        relaxExtended({r.from, e.label, e.to}, r.weight, value);
    });
    auto right = waiting_.find({e.from, e.label});
    if (right != waiting_.end()) {
      for (const auto &waiting : right->second)
        joinPushDelta(waiting, edge_id, *delta, false);
    }
    auto wildcard_right = waiting_.find({e.from, Epsilon});
    if (wildcard_right != waiting_.end()) {
      for (const auto &waiting : wildcard_right->second)
        joinPushDelta(waiting, edge_id, *delta, false);
    }
  }
  const System &system_;
  std::shared_ptr<const typename System::CompiledRuleIndexes> base_indexes_;
  std::vector<typename System::Rule> added_rules_;
  Automaton<Domain> result_;
  const std::size_t base_rule_count_;
  Limits limits_;
  std::deque<TransitionId> queue_;
  std::vector<std::unique_ptr<DeltaPage>> pending_delta_pages_;
  std::deque<Delta> delta_arena_;
  std::vector<DeltaSlot> free_delta_slots_;
  std::unordered_map<Key, std::vector<std::size_t>, KeyHash> indexed_;
  std::vector<std::vector<std::size_t>> wildcard_indexed_;
  std::vector<State> base_generated_;
  std::unordered_map<Key, State, KeyHash> generated_;
  std::unordered_map<Key, std::vector<Waiting>, KeyHash> waiting_;
  std::unordered_set<Waiting, WaitingHash> registered_waiting_;
  bool complete_ = false, poisoned_ = false;
};

template <class Domain>
Automaton<Domain> postStar(const PushdownSystem<Domain> &system,
                           const RegularSet &seed, Limits limits = {}) {
  SaturationSession<Domain> session(system, seed, Direction::Post, limits);
  session.run();
  return session.takeResult();
}
template <class Domain>
Automaton<Domain> preStar(const PushdownSystem<Domain> &system,
                          const RegularSet &target, Limits limits = {}) {
  SaturationSession<Domain> session(system, target, Direction::Pre, limits);
  session.run();
  return session.takeResult();
}

} // namespace lotus::cfl::interleaved_dyck::spds

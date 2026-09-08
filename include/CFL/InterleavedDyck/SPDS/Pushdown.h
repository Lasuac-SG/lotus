#pragma once

#include "CFL/InterleavedDyck/SPDS/Semiring.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
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
    std::size_t seed = std::hash<State>{}(edge.from);
    seed ^= std::hash<Symbol>{}(edge.label) + 0x9e3779b9U + (seed << 6U) +
            (seed >> 2U);
    seed ^=
        std::hash<State>{}(edge.to) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    return seed;
  }
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
  enum class RuleKind { Exact, PreserveAny, PushAny };
  struct Rule {
    RuleKind kind;
    State from;
    Symbol top;
    State to;
    std::vector<Symbol> replacement; // [], [a], or [a,b], top first
    Weight weight;
  };
  explicit PushdownSystem(Domain domain = Domain())
      : domain_(std::move(domain)) {}
  State addControl() { return controls_++; }
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
    rules_.push_back(
        {RuleKind::Exact, from, top, to, std::move(replacement), weight});
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

private:
  void addWildcardRule(RuleKind kind, State from, State to, Symbol pushed,
                       const Weight &weight) {
    if (from >= controls_ || to >= controls_)
      throw std::out_of_range("PDS control");
    (void)domain_.combine(domain_.zero(), weight);
    std::vector<Symbol> replacement;
    if (kind == RuleKind::PushAny)
      replacement.push_back(pushed);
    rules_.push_back({kind, from, 0, to, std::move(replacement), weight});
  }
  Domain domain_;
  std::size_t controls_ = 0;
  std::vector<Rule> rules_;
};

template <class Domain> class SaturationSession;

template <class Domain = BooleanSemiring> class Automaton {
public:
  using Weight = typename Domain::Weight;
  Automaton(const Automaton &) = delete;
  Automaton &operator=(const Automaton &) = delete;
  Automaton(Automaton &&) = default;
  Automaton &operator=(Automaton &&) = default;
  const std::unordered_map<Transition, Weight, TransitionHash> &
  transitions() const {
    return edges_;
  }
  const std::set<State> &finals() const { return finals_; }
  const Statistics &statistics() const { return stats_; }
  const Domain &domain() const { return domain_; }
  std::size_t controls() const { return controls_; }
  std::size_t states() const { return out_.size(); }
  Direction direction() const { return direction_; }

  Weight weight(State control, const std::vector<Symbol> &stack) const {
    auto active = consume(control, stack);
    return acceptingWeight(active);
  }
  bool accepts(State control, const std::vector<Symbol> &stack) const {
    return weight(control, stack) != domain_.zero();
  }
  // Aggregate all accepted stacks beginning with prefix; {} means any stack.
  // This is NOT the same query as accepts(control,{}), which requires empty.
  Weight weightWithPrefix(State control,
                          const std::vector<Symbol> &prefix) const {
    auto active = consume(control, prefix);
    close(active, false);
    return acceptingWeight(active);
  }
  bool acceptsPrefix(State control, const std::vector<Symbol> &prefix) const {
    return weightWithPrefix(control, prefix) != domain_.zero();
  }
  // Evaluate one word/prefix for every PDS control with a single reverse
  // weighted fixed point, rather than restarting a forward closure per control.
  std::vector<Weight> controlWeights(const std::vector<Symbol> &word) const {
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
    return suffix;
  }
  std::vector<Weight>
  controlWeightsWithPrefix(const std::vector<Symbol> &prefix) const {
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
    return suffix;
  }

private:
  friend class SaturationSession<Domain>;
  struct Adjacent {
    Transition edge;
    Weight *weight;
  };
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
      for (const auto &entry : in_[to]) {
        const auto &edge = entry.edge;
        if (epsilon_only && edge.label != Epsilon)
          continue;
        const Weight candidate = prepend(*entry.weight, suffix);
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
      for (const auto &entry : in_[to]) {
        const auto &edge = entry.edge;
        if (edge.label == label) {
          const Weight candidate = prepend(*entry.weight, suffix[to]);
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
      for (const auto &entry : out_[from]) {
        const auto &edge = entry.edge;
        if (epsilon_only && edge.label != Epsilon)
          continue;
        Weight candidate = pathProduct(source, *entry.weight);
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
        for (const auto &adjacent : out_[entry.first]) {
          const auto &edge = adjacent.edge;
          if (edge.label == a) {
            const Weight candidate =
                pathProduct(entry.second, *adjacent.weight);
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
  std::unordered_map<Transition, Weight, TransitionHash> edges_;
  std::vector<std::vector<Adjacent>> out_, in_;
  std::set<State> finals_;
  Statistics stats_;
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
  SaturationSession(const System &system, const RegularSet &seed,
                    Direction direction = Direction::Post, Limits limits = {})
      : system_(system), result_(system.domain(), system.controls(), direction),
        base_rule_count_(system.rules().size()), limits_(limits),
        wildcard_indexed_(system.controls()) {
    if (seed.controls() != system.controls())
      throw std::invalid_argument("seed/PDS control count mismatch");
    result_.edges_.max_load_factor(0.7F);
    if constexpr (!std::is_same_v<Weight, bool>)
      queued_.max_load_factor(0.7F);
    const std::size_t controls = system.controls();
    if (controls >= 256 &&
        controls <= (std::numeric_limits<std::size_t>::max() -
                     seed.transitions().size()) /
                        24) {
      std::size_t expected = controls * 24 + seed.transitions().size();
      if (limits_.max_transitions)
        expected = std::min(expected, limits_.max_transitions);
      result_.edges_.reserve(expected);
      if constexpr (!std::is_same_v<Weight, bool>)
        queued_.reserve(expected);
    }
    if (base_rule_count_ >= 512) {
      indexed_.reserve(base_rule_count_);
      generated_.reserve(base_rule_count_);
    }
    // Clone ALL seed states. No transition enters an original PDS control in
    // the initial automaton, even when the supplied regular seed has such
    // edges.
    for (State p = 0; p < system.controls(); ++p)
      addState();
    const State offset = result_.states();
    for (State q = 0; q < seed.states(); ++q)
      addState();
    for (State q : seed.finals())
      result_.finals_.insert(offset + q);
    for (const auto &e : seed.transitions())
      relax({offset + e.from, e.label, offset + e.to}, domain().one());
    for (State p = 0; p < system.controls(); ++p)
      relax({p, Epsilon, offset + p}, domain().one());
    for (std::size_t i = 0; i < base_rule_count_; ++i)
      installRule(i, false);
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
                              std::move(replacement), weight});
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
    try {
      while (!queue_.empty()) {
        const Pending pending = queue_.front();
        queue_.pop_front();
        const Transition &edge = pending.edge;
        if constexpr (!std::is_same_v<Weight, bool>)
          queued_.erase(edge);
        ++result_.stats_.processed;
        const Weight value = *pending.weight;
        propagateEpsilon(edge, value);
        if (edge.label != Epsilon) {
          if (result_.direction_ == Direction::Post)
            propagatePost(edge, value);
          else
            propagatePre(edge, value);
        }
      }
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
  using Key = std::pair<State, Symbol>;
  struct KeyHash {
    std::size_t operator()(const Key &key) const {
      std::size_t seed = std::hash<State>{}(key.first);
      seed ^= std::hash<Symbol>{}(key.second) + 0x9e3779b9U + (seed << 6U) +
              (seed >> 2U);
      return seed;
    }
  };
  struct Waiting {
    std::size_t rule;
    Transition first;
    bool operator<(const Waiting &other) const {
      if (rule != other.rule)
        return rule < other.rule;
      return first < other.first;
    }
  };
  struct Pending {
    Transition edge;
    Weight *weight;
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
    result_.stats_.states = result_.states();
    return next;
  }
  void schedule(const Transition &edge, Weight *weight) {
    if constexpr (std::is_same_v<Weight, bool>) {
      queue_.push_back({edge, weight});
    } else if (queued_.insert(edge).second) {
      queue_.push_back({edge, weight});
    }
  }
  void relax(const Transition &edge, const Weight &candidate) {
    if (candidate == domain().zero())
      return;
    auto it = result_.edges_.find(edge);
    Weight *stored = nullptr;
    if (it == result_.edges_.end()) {
      if (limits_.max_transitions &&
          result_.edges_.size() >= limits_.max_transitions)
        throw ResourceLimit("SPDS transition limit exceeded");
      if (limits_.max_updates && result_.stats_.updates >= limits_.max_updates)
        throw ResourceLimit("SPDS weight-update limit exceeded");
      auto inserted = result_.edges_.emplace(edge, candidate);
      stored = &inserted.first->second;
      result_.out_[edge.from].push_back({edge, &inserted.first->second});
      result_.in_[edge.to].push_back({edge, &inserted.first->second});
    } else {
      if (limits_.max_updates) {
        Weight joined = it->second;
        if (!domain().combineWith(joined, candidate))
          return;
        if (result_.stats_.updates >= limits_.max_updates)
          throw ResourceLimit("SPDS weight-update limit exceeded");
        it->second = std::move(joined);
      } else if (!domain().combineWith(it->second, candidate)) {
        return;
      }
      stored = &it->second;
    }
    ++result_.stats_.updates;
    result_.stats_.transitions = result_.edges_.size();
    schedule(edge, stored);
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
          if (!generated_.count(generated))
            generated_.emplace(generated, addState());
        }
        if (schedule_existing)
          for (const auto &e : result_.out_[r.from])
            if (e.edge.label != Epsilon)
              schedule(e.edge, e.weight);
        return;
      }
      key = {r.from, r.top};
      if (r.replacement.size() == 2) {
        Key generated{r.to, r.replacement[0]};
        if (!generated_.count(generated))
          generated_.emplace(generated, addState());
      }
    } else {
      if (r.kind == System::RuleKind::PreserveAny) {
        wildcard_indexed_[r.to].push_back(i);
        if (schedule_existing)
          for (const auto &e : result_.out_[r.to])
            if (e.edge.label != Epsilon)
              schedule(e.edge, e.weight);
        return;
      }
      if (r.kind == System::RuleKind::PushAny) {
        key = {r.to, r.replacement[0]};
        indexed_[key].push_back(i);
        if (schedule_existing)
          for (const auto &e : result_.out_[key.first])
            if (e.edge.label == key.second)
              schedule(e.edge, e.weight);
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
      for (const auto &e : result_.out_[key.first])
        if (e.edge.label == key.second)
          schedule(e.edge, e.weight);
  }
  void propagateEpsilon(const Transition &e, const Weight &value) {
    // Capture the old size and reload by index: relax() may reallocate an
    // adjacency vector, but newly appended edges are scheduled separately.
    const std::size_t incoming_size = result_.in_[e.from].size();
    for (std::size_t i = 0; i < incoming_size; ++i) {
      const auto entry = result_.in_[e.from][i];
      const auto &left = entry.edge;
      if (left.label == Epsilon || e.label == Epsilon)
        relax({left.from, left.label == Epsilon ? e.label : left.label, e.to},
              result_.pathProduct(*entry.weight, value));
    }
    const std::size_t outgoing_size = result_.out_[e.to].size();
    for (std::size_t i = 0; i < outgoing_size; ++i) {
      const auto entry = result_.out_[e.to][i];
      const auto &right = entry.edge;
      if (e.label == Epsilon || right.label == Epsilon)
        relax({e.from, e.label == Epsilon ? right.label : e.label, right.to},
              result_.pathProduct(value, *entry.weight));
    }
  }
  void propagatePost(const Transition &e, const Weight &value) {
    auto found = indexed_.find({e.from, e.label});
    if (found != indexed_.end())
      for (std::size_t i : found->second) {
        const auto &r = rule(i);
        Weight extended = domain().extend(value, r.weight);
        if (r.replacement.empty())
          relax({r.to, Epsilon, e.to}, extended);
        else if (r.replacement.size() == 1)
          relax({r.to, r.replacement[0], e.to}, extended);
        else {
          State mid = generated_.at({r.to, r.replacement[0]});
          // The shared first edge carries ONE. Prior history and rule weight
          // belong on the continuation edge; mixing them loses correlations.
          relax({r.to, r.replacement[0], mid}, domain().one());
          relax({mid, r.replacement[1], e.to}, extended);
        }
      }
    if (e.from >= wildcard_indexed_.size())
      return;
    for (std::size_t i : wildcard_indexed_[e.from]) {
      const auto &r = rule(i);
      Weight extended = domain().extend(value, r.weight);
      if (r.kind == System::RuleKind::PreserveAny) {
        relax({r.to, e.label, e.to}, extended);
      } else {
        State mid = generated_.at({r.to, r.replacement[0]});
        relax({r.to, r.replacement[0], mid}, domain().one());
        relax({mid, e.label, e.to}, extended);
      }
    }
  }
  void joinPush(const Waiting &waiting, const Transition &second) {
    const auto &r = rule(waiting.rule);
    const Symbol top =
        r.kind == System::RuleKind::PushAny ? second.label : r.top;
    relax({r.from, top, second.to},
          domain().extend(r.weight,
                          domain().extend(result_.edges_.at(waiting.first),
                                          result_.edges_.at(second))));
  }
  void propagatePre(const Transition &e, const Weight &value) {
    auto found = indexed_.find({e.from, e.label});
    if (found != indexed_.end()) {
      for (std::size_t i : found->second) {
        const auto &r = rule(i);
        if (r.kind == System::RuleKind::PushAny) {
          Waiting waiting{i, e};
          wildcard_waiting_[e.to].insert(waiting);
          const std::size_t count = result_.out_[e.to].size();
          for (std::size_t j = 0; j < count; ++j) {
            const auto second = result_.out_[e.to][j];
            if (second.edge.label != Epsilon)
              joinPush(waiting, second.edge);
          }
        } else if (r.replacement.size() == 1)
          relax({r.from, r.top, e.to}, domain().extend(r.weight, value));
        else {
          Waiting waiting{i, e};
          waiting_[{e.to, r.replacement[1]}].insert(waiting);
          const std::size_t count = result_.out_[e.to].size();
          for (std::size_t j = 0; j < count; ++j) {
            const auto second = result_.out_[e.to][j];
            if (second.edge.label == r.replacement[1])
              joinPush(waiting, second.edge);
          }
        }
      }
    }
    if (e.from < wildcard_indexed_.size())
      for (std::size_t i : wildcard_indexed_[e.from]) {
        const auto &r = rule(i);
        relax({r.from, e.label, e.to}, domain().extend(r.weight, value));
      }
    auto right = waiting_.find({e.from, e.label});
    if (right != waiting_.end()) {
      for (const auto &waiting : right->second)
        joinPush(waiting, e);
    }
    auto wildcard_right = wildcard_waiting_.find(e.from);
    if (wildcard_right != wildcard_waiting_.end()) {
      for (const auto &waiting : wildcard_right->second)
        joinPush(waiting, e);
    }
  }
  const System &system_;
  std::vector<typename System::Rule> added_rules_;
  Automaton<Domain> result_;
  const std::size_t base_rule_count_;
  Limits limits_;
  std::deque<Pending> queue_;
  std::unordered_set<Transition, TransitionHash> queued_;
  std::unordered_map<Key, std::vector<std::size_t>, KeyHash> indexed_;
  std::vector<std::vector<std::size_t>> wildcard_indexed_;
  std::unordered_map<Key, State, KeyHash> generated_;
  std::unordered_map<Key, std::set<Waiting>, KeyHash> waiting_;
  std::unordered_map<State, std::set<Waiting>> wildcard_waiting_;
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

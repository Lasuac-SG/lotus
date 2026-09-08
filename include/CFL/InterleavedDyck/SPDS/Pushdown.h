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
  std::size_t states = 0, transitions = 0, updates = 0, processed = 0, rules = 0;
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
};

// Unweighted regular seed/target set. States [0,controls) are PDS controls.
// Incoming edges to controls, epsilon edges, and accepting controls are allowed:
// saturation normalizes a private copy before computing post* or pre*.
class RegularSet {
public:
  explicit RegularSet(std::size_t controls) : controls_(controls), states_(controls) {}
  State addState() { return states_++; }
  void addFinal(State q) { check(q); finals_.insert(q); }
  void addTransition(State from, Symbol label, State to) {
    check(from); check(to); edges_.push_back({from, label, to});
  }
  static RegularSet singleton(std::size_t controls, const Configuration &c) {
    if (c.control >= controls) throw std::out_of_range("seed control");
    RegularSet result(controls);
    State q = c.control;
    if (c.stack.empty()) {
      result.addFinal(q);
    } else {
      for (Symbol a : c.stack) {
        if (a == Epsilon) throw std::invalid_argument("epsilon in seed stack");
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
    if (q >= states_) throw std::out_of_range("seed automaton state");
  }
  std::size_t controls_, states_;
  std::vector<Transition> edges_;
  std::set<State> finals_;
};

template <class Domain = BooleanSemiring> class PushdownSystem {
public:
  using Weight = typename Domain::Weight;
  struct Rule {
    State from;
    Symbol top;
    State to;
    std::vector<Symbol> replacement; // [], [a], or [a,b], top first
    Weight weight;
  };
  explicit PushdownSystem(Domain domain = Domain()) : domain_(std::move(domain)) {}
  State addControl() { return controls_++; }
  void addRule(State from, Symbol top, State to,
               std::vector<Symbol> replacement, const Weight &weight) {
    if (from >= controls_ || to >= controls_) throw std::out_of_range("PDS control");
    if (top == Epsilon || replacement.size() > 2 ||
        std::find(replacement.begin(), replacement.end(), Epsilon) != replacement.end())
      throw std::invalid_argument("PDS rule must replace one symbol by at most two");
    // Also catches incompatible weights in domains such as RelationSemiring.
    (void)domain_.combine(domain_.zero(), weight);
    rules_.push_back({from, top, to, std::move(replacement), weight});
  }
  void addRule(State from, Symbol top, State to, std::vector<Symbol> replacement) {
    addRule(from, top, to, std::move(replacement), domain_.one());
  }
  std::size_t controls() const { return controls_; }
  const std::vector<Rule> &rules() const { return rules_; }
  const Domain &domain() const { return domain_; }
private:
  Domain domain_;
  std::size_t controls_ = 0;
  std::vector<Rule> rules_;
};

template <class Domain> class SaturationSession;

template <class Domain = BooleanSemiring> class Automaton {
public:
  using Weight = typename Domain::Weight;
  const std::map<Transition, Weight> &transitions() const { return edges_; }
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
  Weight weightWithPrefix(State control, const std::vector<Symbol> &prefix) const {
    auto active = consume(control, prefix);
    close(active, false);
    return acceptingWeight(active);
  }
  bool acceptsPrefix(State control, const std::vector<Symbol> &prefix) const {
    return weightWithPrefix(control, prefix) != domain_.zero();
  }
private:
  friend class SaturationSession<Domain>;
  Automaton(Domain domain, std::size_t controls, Direction direction)
      : domain_(std::move(domain)), controls_(controls), direction_(direction) {}
  // post* automaton paths encode execution from right to left. pre* paths
  // encode execution from left to right. Never assume extend is commutative.
  Weight pathProduct(const Weight &left, const Weight &right) const {
    return direction_ == Direction::Post ? domain_.extend(right, left)
                                         : domain_.extend(left, right);
  }
  using Valuation = std::map<State, Weight>;
  void close(Valuation &active, bool epsilon_only) const {
    std::deque<State> queue;
    std::set<State> pending;
    for (const auto &entry : active) {
      queue.push_back(entry.first); pending.insert(entry.first);
    }
    while (!queue.empty()) {
      State from = queue.front(); queue.pop_front(); pending.erase(from);
      const Weight source = active.at(from);
      for (const auto &edge : out_[from]) {
        if (epsilon_only && edge.label != Epsilon) continue;
        Weight candidate = pathProduct(source, edges_.at(edge));
        if (candidate == domain_.zero()) continue;
        auto it = active.find(edge.to);
        Weight old = it == active.end() ? domain_.zero() : it->second;
        Weight joined = domain_.combine(old, candidate);
        if (joined != old) {
          active.insert_or_assign(edge.to, joined);
          if (pending.insert(edge.to).second) queue.push_back(edge.to);
        }
      }
    }
  }
  Valuation consume(State control, const std::vector<Symbol> &word) const {
    if (control >= controls_) throw std::out_of_range("query control");
    Valuation active;
    active.emplace(control, domain_.one());
    close(active, true);
    for (Symbol a : word) {
      if (a == Epsilon) throw std::invalid_argument("epsilon in query stack");
      Valuation next;
      for (const auto &entry : active)
        for (const auto &edge : out_[entry.first])
          if (edge.label == a) {
            const Weight candidate = pathProduct(entry.second, edges_.at(edge));
            if (candidate == domain_.zero()) continue;
            auto it = next.find(edge.to);
            Weight old = it == next.end() ? domain_.zero() : it->second;
            next.insert_or_assign(edge.to, domain_.combine(old, candidate));
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
      if (it != active.end()) result = domain_.combine(result, it->second);
    }
    return result;
  }
  Domain domain_;
  std::size_t controls_;
  Direction direction_;
  std::map<Transition, Weight> edges_;
  std::vector<std::vector<Transition>> out_, in_;
  std::set<State> finals_;
  Statistics stats_;
};

// Incremental weighted post*/pre* saturation. Controls are fixed; clients can
// monotonically add rules (e.g. alias-induced flows) and resume run(). Every
// improved weight is rescheduled, not just every newly discovered transition.
// Domain must be a finite-height idempotent semiring; zero must annihilate extend.
template <class Domain = BooleanSemiring> class SaturationSession {
public:
  using System = PushdownSystem<Domain>;
  using Weight = typename Domain::Weight;
  SaturationSession(const System &system, const RegularSet &seed,
                    Direction direction = Direction::Post, Limits limits = {})
      : system_(system), result_(system.domain(), system.controls(), direction),
        limits_(limits) {
    if (seed.controls() != system.controls())
      throw std::invalid_argument("seed/PDS control count mismatch");
    // Clone ALL seed states. No transition enters an original PDS control in
    // the initial automaton, even when the supplied regular seed has such edges.
    for (State p = 0; p < system.controls(); ++p) addState();
    const State offset = result_.states();
    for (State q = 0; q < seed.states(); ++q) addState();
    for (State q : seed.finals()) result_.finals_.insert(offset + q);
    for (const auto &e : seed.transitions())
      relax({offset + e.from, e.label, offset + e.to}, domain().one());
    for (State p = 0; p < system.controls(); ++p)
      relax({p, Epsilon, offset + p}, domain().one());
    for (std::size_t i = 0; i < system_.rules().size(); ++i) installRule(i);
  }
  void addRule(State from, Symbol top, State to, std::vector<Symbol> replacement,
               const Weight &weight) {
    checkUsable();
    complete_ = false;
    try {
      system_.addRule(from, top, to, std::move(replacement), weight);
      installRule(system_.rules().size() - 1);
    } catch (...) { poisoned_ = true; throw; }
  }
  void addRule(State from, Symbol top, State to, std::vector<Symbol> replacement) {
    addRule(from, top, to, std::move(replacement), domain().one());
  }
  const Automaton<Domain> &run() {
    checkUsable();
    complete_ = false;
    try {
      while (!queue_.empty()) {
        Transition edge = queue_.front(); queue_.pop_front(); queued_.erase(edge);
        ++result_.stats_.processed;
        propagateEpsilon(edge);
        if (edge.label != Epsilon) {
          if (result_.direction_ == Direction::Post) propagatePost(edge);
          else propagatePre(edge);
        }
      }
      complete_ = true;
      return result_;
    } catch (...) { poisoned_ = true; throw; }
  }
  const Automaton<Domain> &result() const {
    checkUsable();
    if (!complete_) throw std::logic_error("SPDS saturation is not complete");
    return result_;
  }
private:
  using Key = std::pair<State, Symbol>;
  struct Waiting {
    std::size_t rule;
    Transition first;
    bool operator<(const Waiting &other) const {
      if (rule != other.rule) return rule < other.rule;
      return first < other.first;
    }
  };
  const Domain &domain() const { return system_.domain(); }
  void checkUsable() const {
    if (poisoned_) throw std::logic_error("failed SPDS session; discard and restart");
  }
  State addState() {
    if (limits_.max_states && result_.states() >= limits_.max_states)
      throw ResourceLimit("SPDS state limit exceeded");
    State next = result_.states();
    result_.out_.emplace_back(); result_.in_.emplace_back();
    result_.stats_.states = result_.states();
    return next;
  }
  void schedule(const Transition &edge) {
    if (queued_.insert(edge).second) queue_.push_back(edge);
  }
  void relax(const Transition &edge, const Weight &candidate) {
    if (candidate == domain().zero()) return;
    auto it = result_.edges_.find(edge);
    Weight old = it == result_.edges_.end() ? domain().zero() : it->second;
    Weight joined = domain().combine(old, candidate);
    if (joined == old) return;
    if (limits_.max_updates && result_.stats_.updates >= limits_.max_updates)
      throw ResourceLimit("SPDS weight-update limit exceeded");
    if (it == result_.edges_.end()) {
      if (limits_.max_transitions && result_.edges_.size() >= limits_.max_transitions)
        throw ResourceLimit("SPDS transition limit exceeded");
      result_.out_[edge.from].push_back(edge);
      result_.in_[edge.to].push_back(edge);
    }
    result_.edges_.insert_or_assign(edge, joined);
    ++result_.stats_.updates;
    result_.stats_.transitions = result_.edges_.size();
    schedule(edge);
  }
  void installRule(std::size_t i) {
    const auto &r = system_.rules()[i];
    result_.stats_.rules = system_.rules().size();
    if (r.weight == domain().zero()) return;
    Key key;
    if (result_.direction_ == Direction::Post) {
      key = {r.from, r.top};
      if (r.replacement.size() == 2) {
        Key generated{r.to, r.replacement[0]};
        if (!generated_.count(generated)) generated_.emplace(generated, addState());
      }
    } else {
      if (r.replacement.empty()) {
        relax({r.from, r.top, r.to}, r.weight);
        return;
      }
      key = {r.to, r.replacement[0]};
    }
    indexed_[key].push_back(i);
    for (const auto &e : result_.out_[key.first])
      if (e.label == key.second) schedule(e);
  }
  void propagateEpsilon(const Transition &e) {
    const Weight value = result_.edges_.at(e);
    // Snapshot: relax() can reallocate adjacency lists, even on self loops.
    const auto incoming = result_.in_[e.from];
    for (const auto &left : incoming)
      if (left.label == Epsilon || e.label == Epsilon)
        relax({left.from, left.label == Epsilon ? e.label : left.label, e.to},
              result_.pathProduct(result_.edges_.at(left), value));
    const auto outgoing = result_.out_[e.to];
    for (const auto &right : outgoing)
      if (e.label == Epsilon || right.label == Epsilon)
        relax({e.from, e.label == Epsilon ? right.label : e.label, right.to},
              result_.pathProduct(value, result_.edges_.at(right)));
  }
  void propagatePost(const Transition &e) {
    auto found = indexed_.find({e.from, e.label});
    if (found == indexed_.end()) return;
    const Weight value = result_.edges_.at(e);
    for (std::size_t i : found->second) {
      const auto &r = system_.rules()[i];
      Weight extended = domain().extend(value, r.weight);
      if (r.replacement.empty()) relax({r.to, Epsilon, e.to}, extended);
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
  }
  void joinPush(const Waiting &waiting, const Transition &second) {
    const auto &r = system_.rules()[waiting.rule];
    relax({r.from, r.top, second.to}, domain().extend(r.weight,
          domain().extend(result_.edges_.at(waiting.first), result_.edges_.at(second))));
  }
  void propagatePre(const Transition &e) {
    auto found = indexed_.find({e.from, e.label});
    if (found != indexed_.end()) {
      for (std::size_t i : found->second) {
        const auto &r = system_.rules()[i];
        if (r.replacement.size() == 1)
          relax({r.from, r.top, e.to}, domain().extend(r.weight, result_.edges_.at(e)));
        else {
          Waiting waiting{i, e};
          waiting_[{e.to, r.replacement[1]}].insert(waiting);
          const auto outgoing = result_.out_[e.to];
          for (const auto &second : outgoing)
            if (second.label == r.replacement[1]) joinPush(waiting, second);
        }
      }
    }
    auto right = waiting_.find({e.from, e.label});
    if (right != waiting_.end()) {
      const auto snapshot = right->second;
      for (const auto &waiting : snapshot) joinPush(waiting, e);
    }
  }
  System system_;
  Automaton<Domain> result_;
  Limits limits_;
  std::deque<Transition> queue_;
  std::set<Transition> queued_;
  std::map<Key, std::vector<std::size_t>> indexed_;
  std::map<Key, State> generated_;
  std::map<Key, std::set<Waiting>> waiting_;
  bool complete_ = false, poisoned_ = false;
};

template <class Domain>
Automaton<Domain> postStar(const PushdownSystem<Domain> &system,
                          const RegularSet &seed, Limits limits = {}) {
  SaturationSession<Domain> session(system, seed, Direction::Post, limits);
  return session.run();
}
template <class Domain>
Automaton<Domain> preStar(const PushdownSystem<Domain> &system,
                         const RegularSet &target, Limits limits = {}) {
  SaturationSession<Domain> session(system, target, Direction::Pre, limits);
  return session.run();
}

} // namespace lotus::cfl::interleaved_dyck::spds

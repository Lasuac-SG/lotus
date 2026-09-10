#pragma once

#include "CFL/InterleavedDyck/AffineSPDS/Solver.h"
#include "CFL/InterleavedDyck/SPDS/Synchronized.h"

namespace lotus::cfl::interleaved_dyck::affine {
using spds::Field;
using spds::FlowNode;
using spds::Statement;
using spds::SynchronizedConfiguration;
using spds::Variable;

class SynchronizedSystem;
class SynchronizedResult {
public:
  const spds::Automaton<AffineSemiring> &callAutomaton() const {
    return calls_;
  }
  const spds::Automaton<AffineSemiring> &fieldAutomaton() const {
    return fields_;
  }
  HistoryComparison compare(const SynchronizedConfiguration &query) const {
    auto node = locations_.find(query.node);
    std::vector<spds::Symbol> call_word, field_word;
    if (node == locations_.end() || !encodeFields(query.fields, field_word))
      return bottom();
    call_word.push_back(statements_.at(query.node.statement));
    for (Statement s : query.context) {
      auto found = statements_.find(s);
      if (found == statements_.end())
        return bottom();
      call_word.push_back(found->second);
    }
    return HistoryComparison(
        read(true, variables_.at(query.node.variable), call_word, false),
        read(false, node->second, field_word, false));
  }
  bool mayAccept(const SynchronizedConfiguration &query) const {
    return compare(query).mayReach();
  }
  // Exact access path, existential calling context. Histories on both sides
  // are synchronized BEFORE a may-alias answer is returned.
  HistoryComparison compareAt(FlowNode node,
                              const std::vector<Field> &fields = {}) const {
    auto found = locations_.find(node);
    std::vector<spds::Symbol> word;
    if (found == locations_.end() || !encodeFields(fields, word))
      return bottom();
    return HistoryComparison(read(true, variables_.at(node.variable),
                                  {statements_.at(node.statement)}, true),
                             read(false, found->second, word, false));
  }
  bool mayAlias(FlowNode node) const { return compareAt(node).mayReach(); }
  HistoryComparison compareNode(FlowNode node) const {
    auto found = locations_.find(node);
    if (found == locations_.end())
      return bottom();
    return HistoryComparison(read(true, variables_.at(node.variable),
                                  {statements_.at(node.statement)}, true),
                             read(false, found->second, {}, true));
  }
  bool mayReachNode(FlowNode node) const {
    return compareNode(node).mayReach();
  }

private:
  friend class SynchronizedSystem;
  using ReadKey =
      std::tuple<bool, bool, spds::State, std::vector<spds::Symbol>>;
  // Bounded per-result memoization; const queries on one result are serialized
  // by the caller, as for QueryResult.
  mutable std::map<ReadKey, AffineSpace> read_cache_;
  AffineSpace read(bool calls, spds::State control,
                   const std::vector<spds::Symbol> &word, bool prefix) const {
    const ReadKey key{calls, prefix, control, word};
    if (const auto found = read_cache_.find(key); found != read_cache_.end())
      return found->second;
    const auto &automaton = calls ? calls_ : fields_;
    auto result = prefix ? automaton.weightWithPrefix(control, word)
                         : automaton.weight(control, word);
    if (read_cache_.size() == 64)
      read_cache_.erase(read_cache_.begin());
    read_cache_.emplace(key, result);
    return result;
  }
  SynchronizedResult(spds::Automaton<AffineSemiring> calls,
                     spds::Automaton<AffineSemiring> fields,
                     std::map<Variable, spds::State> variables,
                     std::map<FlowNode, spds::State> locations,
                     std::map<Statement, spds::Symbol> statements,
                     std::map<Field, spds::Symbol> field_ids)
      : calls_(std::move(calls)), fields_(std::move(fields)),
        variables_(std::move(variables)), locations_(std::move(locations)),
        statements_(std::move(statements)), field_ids_(std::move(field_ids)) {}
  HistoryComparison bottom() const {
    return HistoryComparison(calls_.domain().zero(), fields_.domain().zero());
  }
  bool encodeFields(const std::vector<Field> &fields,
                    std::vector<spds::Symbol> &word) const {
    for (auto f : fields) {
      auto found = field_ids_.find(f);
      if (found == field_ids_.end())
        return false;
      word.push_back(found->second);
    }
    word.push_back(0);
    return true;
  }
  spds::Automaton<AffineSemiring> calls_, fields_;
  std::map<Variable, spds::State> variables_;
  std::map<FlowNode, spds::State> locations_;
  std::map<Statement, spds::Symbol> statements_;
  std::map<Field, spds::Symbol> field_ids_;
};

// Paper-style variable/statement encoding with affine history on BOTH PDSs.
// A transfer is one data-flow edge. Its matrix is shared across all projected
// rules. Clients supply flow functions/aliases, not a different algebra.
// This does not perform alias discovery or justify strong updates by itself.
class SynchronizedSystem {
public:
  explicit SynchronizedSystem(std::size_t dimension = 1) : domain_(dimension) {}
  void addNode(FlowNode node) { nodes_.insert(node); }
  void addNormal(FlowNode from, FlowNode to, const Matrix &m) {
    add(Kind::Normal, from, to, 0, 0, m);
  }
  void addStore(FlowNode from, FlowNode to, Field field, const Matrix &m) {
    add(Kind::Store, from, to, field, 0, m);
  }
  void addLoad(FlowNode from, FlowNode to, Field field, const Matrix &m) {
    add(Kind::Load, from, to, field, 0, m);
  }
  void addCall(FlowNode from, FlowNode entry, Statement return_site,
               const Matrix &m) {
    add(Kind::Call, from, entry, 0, return_site, m);
  }
  void addReturn(FlowNode exit, FlowNode to, const Matrix &m) {
    add(Kind::Return, exit, to, 0, 0, m);
  }
  void addNormal(FlowNode a, FlowNode b) { addNormal(a, b, identity()); }
  void addStore(FlowNode a, FlowNode b, Field f) {
    addStore(a, b, f, identity());
  }
  void addLoad(FlowNode a, FlowNode b, Field f) {
    addLoad(a, b, f, identity());
  }
  void addCall(FlowNode a, FlowNode b, Statement s) {
    addCall(a, b, s, identity());
  }
  void addReturn(FlowNode a, FlowNode b) { addReturn(a, b, identity()); }
  SynchronizedResult postStar(const SynchronizedConfiguration &seed,
                              spds::Limits limits = {}) const {
    return analyze(seed, spds::Direction::Post, limits);
  }
  SynchronizedResult preStar(const SynchronizedConfiguration &target,
                             spds::Limits limits = {}) const {
    return analyze(target, spds::Direction::Pre, limits);
  }

private:
  enum class Kind { Normal, Store, Load, Call, Return };
  struct Transfer {
    Kind kind;
    FlowNode from, to;
    Field field;
    Statement return_site;
    AffineSpace weight;
  };
  Matrix identity() const { return Matrix::identity(domain_.dimension()); }
  void add(Kind kind, FlowNode from, FlowNode to, Field field, Statement ret,
           const Matrix &m) {
    auto weight = domain_.lift(m);
    addNode(from);
    addNode(to);
    transfers_.push_back({kind, from, to, field, ret, std::move(weight)});
  }
  SynchronizedResult analyze(const SynchronizedConfiguration &seed,
                             spds::Direction direction,
                             spds::Limits limits) const {
    using spds::State;
    using spds::Symbol;
    auto nodes = nodes_;
    nodes.insert(seed.node);
    std::map<Variable, State> variables;
    std::map<FlowNode, State> locations;
    std::map<Statement, Symbol> statements;
    std::map<Field, Symbol> fields;
    auto statement = [&](Statement s) {
      if (!statements.count(s))
        statements.emplace(s, statements.size() + 1);
    };
    auto field = [&](Field f) {
      if (!fields.count(f))
        fields.emplace(f, fields.size() + 1);
    };
    spds::PushdownSystem<AffineSemiring> calls(domain_), heap(domain_);
    for (FlowNode node : nodes) {
      if (!variables.count(node.variable))
        variables.emplace(node.variable, calls.addControl());
      locations.emplace(node, heap.addControl());
      statement(node.statement);
    }
    for (Statement s : seed.context)
      statement(s);
    for (Field f : seed.fields)
      field(f);
    for (const auto &t : transfers_) {
      if (t.kind == Kind::Call)
        statement(t.return_site);
      if (t.kind == Kind::Store || t.kind == Kind::Load)
        field(t.field);
    }
    for (const auto &t : transfers_) {
      State x = variables.at(t.from.variable), y = variables.at(t.to.variable);
      Symbol s = statements.at(t.from.statement),
             target = statements.at(t.to.statement);
      if (t.kind == Kind::Call)
        calls.addRule(x, s, y, {target, statements.at(t.return_site)},
                      t.weight);
      else if (t.kind == Kind::Return)
        calls.addRule(x, s, y, {}, t.weight);
      else
        calls.addRule(x, s, y, {target}, t.weight);
      State a = locations.at(t.from), b = locations.at(t.to);
      if (t.kind == Kind::Load)
        heap.addRule(a, fields.at(t.field), b, {}, t.weight);
      else if (t.kind == Kind::Store)
        heap.addPushRule(a, b, fields.at(t.field), t.weight);
      else
        heap.addPreserveRule(a, b, t.weight);
    }
    std::vector<Symbol> call_word{statements.at(seed.node.statement)},
        field_word;
    for (Statement s : seed.context)
      call_word.push_back(statements.at(s));
    for (Field f : seed.fields)
      field_word.push_back(fields.at(f));
    field_word.push_back(0);
    auto cseed = spds::RegularSet::singleton(
        calls.controls(), {variables.at(seed.node.variable), call_word});
    auto fseed = spds::RegularSet::singleton(
        heap.controls(), {locations.at(seed.node), field_word});
    auto c = direction == spds::Direction::Post
                 ? spds::postStar(calls, cseed, limits)
                 : spds::preStar(calls, cseed, limits);
    auto f = direction == spds::Direction::Post
                 ? spds::postStar(heap, fseed, limits)
                 : spds::preStar(heap, fseed, limits);
    return SynchronizedResult(std::move(c), std::move(f), std::move(variables),
                              std::move(locations), std::move(statements),
                              std::move(fields));
  }
  AffineSemiring domain_;
  std::set<FlowNode> nodes_;
  std::vector<Transfer> transfers_;
};
} // namespace lotus::cfl::interleaved_dyck::affine

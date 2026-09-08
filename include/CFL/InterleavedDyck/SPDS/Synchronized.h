#pragma once

#include "CFL/InterleavedDyck/SPDS/Pushdown.h"
#include <cstdint>

namespace lotus::cfl::interleaved_dyck::spds {

using Variable = std::int64_t;
using Statement = std::int64_t;
using Field = unsigned;
struct FlowNode {
  Variable variable = 0;
  Statement statement = 0;
  bool operator<(const FlowNode &r) const {
    return std::tie(variable, statement) < std::tie(r.variable, r.statement);
  }
};
struct SynchronizedConfiguration {
  FlowNode node;
  std::vector<Statement> context; // pending return sites, top first
  std::vector<Field> fields;     // access path after the base variable
};

template <class Domain> class SynchronizedSystem;

// Definition 4: two automata, NOT a product of two concrete unbounded stacks.
// A positive result need not have a single path witnessing both projections.
template <class Domain = BooleanSemiring> class SynchronizedResult {
public:
  using Weight = typename Domain::Weight;
  const Automaton<Domain> &callAutomaton() const { return call_; }
  const Automaton<BooleanSemiring> &fieldAutomaton() const { return field_; }
  Weight weight(const SynchronizedConfiguration &query) const {
    auto node = field_controls_.find(query.node);
    if (node == field_controls_.end()) return call_.domain().zero();
    std::vector<Symbol> fields, calls;
    if (!encodeFields(query.fields, fields) ||
        !encodeCalls(query, calls) || !field_.accepts(node->second, fields))
      return call_.domain().zero();
    return call_.weight(call_controls_.at(query.node.variable), calls);
  }
  bool mayAccept(const SynchronizedConfiguration &query) const {
    return weight(query) != call_.domain().zero();
  }
  // Query a specified access path at a statement, in ANY calling context.
  // Empty fields tests the base variable, as required for points-to/alias clients.
  Weight weightAt(FlowNode node, const std::vector<Field> &fields = {}) const {
    auto found = field_controls_.find(node);
    if (found == field_controls_.end()) return call_.domain().zero();
    std::vector<Symbol> word;
    if (!encodeFields(fields, word) || !field_.accepts(found->second, word))
      return call_.domain().zero();
    return call_.weightWithPrefix(call_controls_.at(node.variable),
                                 {statements_.at(node.statement)});
  }
  bool mayAlias(FlowNode node) const {
    return weightAt(node) != call_.domain().zero();
  }
  // Existential calling context AND existential access path.
  bool mayReachNode(FlowNode node) const {
    auto found = field_controls_.find(node);
    return found != field_controls_.end() &&
        field_.acceptsPrefix(found->second, {}) &&
        call_.acceptsPrefix(call_controls_.at(node.variable),
                            {statements_.at(node.statement)});
  }
private:
  friend class SynchronizedSystem<Domain>;
  SynchronizedResult(Automaton<Domain> call, Automaton<BooleanSemiring> field,
                     std::map<Variable, State> call_controls,
                     std::map<FlowNode, State> field_controls,
                     std::map<Statement, Symbol> statements,
                     std::map<Field, Symbol> fields)
      : call_(std::move(call)), field_(std::move(field)),
        call_controls_(std::move(call_controls)), field_controls_(std::move(field_controls)),
        statements_(std::move(statements)), fields_(std::move(fields)) {}
  bool encodeFields(const std::vector<Field> &input, std::vector<Symbol> &out) const {
    for (Field f : input) {
      auto it = fields_.find(f);
      if (it == fields_.end()) return false;
      out.push_back(it->second);
    }
    out.push_back(0); // A protected bottom symbol, NOT automaton epsilon.
    return true;
  }
  bool encodeCalls(const SynchronizedConfiguration &c, std::vector<Symbol> &out) const {
    auto top = statements_.find(c.node.statement);
    if (top == statements_.end()) return false;
    out.push_back(top->second);
    for (Statement s : c.context) {
      auto it = statements_.find(s);
      if (it == statements_.end()) return false;
      out.push_back(it->second);
    }
    return true;
  }
  Automaton<Domain> call_;
  Automaton<BooleanSemiring> field_;
  std::map<Variable, State> call_controls_;
  std::map<FlowNode, State> field_controls_;
  std::map<Statement, Symbol> statements_;
  std::map<Field, Symbol> fields_;
};

// A front-end-neutral form of Sections 2-4. Each transfer represents ONE
// data-flow edge, not an entire program statement. The client supplies identity
// edges, kills, actual/formal mappings, and alias-induced flows explicitly.
// Variables must be globally unique (including method scope).
template <class Domain = BooleanSemiring> class SynchronizedSystem {
public:
  using Weight = typename Domain::Weight;
  explicit SynchronizedSystem(Domain domain = Domain()) : domain_(std::move(domain)) {}
  void addNode(FlowNode node) { nodes_.insert(node); }
  void addNormal(FlowNode from, FlowNode to) { addNormal(from, to, domain_.one()); }
  void addNormal(FlowNode from, FlowNode to, const Weight &w) {
    add(Kind::Normal, from, to, 0, 0, w);
  }
  void addStore(FlowNode from, FlowNode to, Field field) {
    addStore(from, to, field, domain_.one());
  }
  void addStore(FlowNode from, FlowNode to, Field field, const Weight &w) {
    add(Kind::Store, from, to, field, 0, w);
  }
  void addLoad(FlowNode from, FlowNode to, Field field) {
    addLoad(from, to, field, domain_.one());
  }
  void addLoad(FlowNode from, FlowNode to, Field field, const Weight &w) {
    add(Kind::Load, from, to, field, 0, w);
  }
  void addCall(FlowNode from, FlowNode entry, Statement return_site) {
    addCall(from, entry, return_site, domain_.one());
  }
  void addCall(FlowNode from, FlowNode entry, Statement return_site, const Weight &w) {
    add(Kind::Call, from, entry, 0, return_site, w);
  }
  // Call-PDS: pop the exit statement, revealing the saved caller statement;
  // map to to.variable. Field-PDS: normal flow to this possible caller node.
  // As in the paper's Table 2, the call pop rule does NOT test to.statement.
  void addReturn(FlowNode exit, FlowNode to) { addReturn(exit, to, domain_.one()); }
  void addReturn(FlowNode exit, FlowNode to, const Weight &w) {
    add(Kind::Return, exit, to, 0, 0, w);
  }
  SynchronizedResult<Domain> postStar(const SynchronizedConfiguration &seed,
                                      Limits limits = {}) const {
    return analyze(seed, Direction::Post, limits);
  }
  SynchronizedResult<Domain> preStar(const SynchronizedConfiguration &target,
                                     Limits limits = {}) const {
    return analyze(target, Direction::Pre, limits);
  }
private:
  enum class Kind { Normal, Store, Load, Call, Return };
  struct Transfer {
    Kind kind;
    FlowNode from, to;
    Field field;
    Statement return_site;
    Weight weight;
  };
  void add(Kind kind, FlowNode from, FlowNode to, Field field,
           Statement return_site, const Weight &w) {
    (void)domain_.combine(domain_.zero(), w);
    addNode(from); addNode(to);
    transfers_.push_back({kind, from, to, field, return_site, w});
  }
  SynchronizedResult<Domain> analyze(const SynchronizedConfiguration &seed,
                                     Direction direction, Limits limits) const {
    auto nodes = nodes_;
    nodes.insert(seed.node);
    std::map<Variable, State> variables;
    std::map<FlowNode, State> locations;
    std::map<Statement, Symbol> statements;
    std::map<Field, Symbol> fields;
    auto statement = [&](Statement s) {
      if (!statements.count(s)) statements.emplace(s, statements.size() + 1);
    };
    auto field = [&](Field f) {
      if (!fields.count(f)) fields.emplace(f, fields.size() + 1);
    };
    PushdownSystem<Domain> calls(domain_);
    PushdownSystem<BooleanSemiring> heap;
    for (FlowNode node : nodes) {
      if (!variables.count(node.variable)) variables.emplace(node.variable, calls.addControl());
      locations.emplace(node, heap.addControl());
      statement(node.statement);
    }
    for (Statement s : seed.context) statement(s);
    for (Field f : seed.fields) field(f);
    for (const auto &t : transfers_) {
      if (t.kind == Kind::Call) statement(t.return_site);
      if (t.kind == Kind::Store || t.kind == Kind::Load) field(t.field);
    }
    std::vector<Symbol> field_alphabet{0};
    for (const auto &entry : fields) field_alphabet.push_back(entry.second);
    for (const auto &t : transfers_) {
      State x = variables.at(t.from.variable), y = variables.at(t.to.variable);
      Symbol s = statements.at(t.from.statement), target = statements.at(t.to.statement);
      if (t.kind == Kind::Call)
        calls.addRule(x, s, y, {target, statements.at(t.return_site)}, t.weight);
      else if (t.kind == Kind::Return) calls.addRule(x, s, y, {}, t.weight);
      else calls.addRule(x, s, y, {target}, t.weight);
      State a = locations.at(t.from), b = locations.at(t.to);
      if (t.kind == Kind::Load) heap.addRule(a, fields.at(t.field), b, {});
      else
        for (Symbol top : field_alphabet)
          if (t.kind == Kind::Store) heap.addRule(a, top, b, {fields.at(t.field), top});
          else heap.addRule(a, top, b, {top});
    }
    std::vector<Symbol> call_stack{statements.at(seed.node.statement)}, field_stack;
    for (Statement s : seed.context) call_stack.push_back(statements.at(s));
    for (Field f : seed.fields) field_stack.push_back(fields.at(f));
    field_stack.push_back(0);
    auto call_seed = RegularSet::singleton(calls.controls(),
                                         {variables.at(seed.node.variable), call_stack});
    auto field_seed = RegularSet::singleton(heap.controls(),
                                          {locations.at(seed.node), field_stack});
    auto call_result = direction == Direction::Post
        ? spds::postStar(calls, call_seed, limits) : spds::preStar(calls, call_seed, limits);
    auto field_result = direction == Direction::Post
        ? spds::postStar(heap, field_seed, limits) : spds::preStar(heap, field_seed, limits);
    return SynchronizedResult<Domain>(std::move(call_result), std::move(field_result),
        std::move(variables), std::move(locations), std::move(statements), std::move(fields));
  }
  Domain domain_;
  std::set<FlowNode> nodes_;
  std::vector<Transfer> transfers_;
};

} // namespace lotus::cfl::interleaved_dyck::spds

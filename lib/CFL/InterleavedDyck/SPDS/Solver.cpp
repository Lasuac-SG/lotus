#include "CFL/InterleavedDyck/SPDS/Solver.h"

namespace lotus::cfl::interleaved_dyck::spds {
namespace {

// 0 is the protected bottom; uint64_t keeps UINT_MAX+1 representable.
Symbol symbol(unsigned id) { return static_cast<Symbol>(id) + 1; }
std::vector<Symbol> stackWord(const std::vector<unsigned> &stack) {
  std::vector<Symbol> result;
  result.reserve(stack.size() + 1);
  for (unsigned id : stack) result.push_back(symbol(id));
  result.push_back(0);
  return result;
}
struct Projections {
  std::map<Vertex, State> controls;
  PushdownSystem<BooleanSemiring> calls, fields;
};
Projections project(const Graph &graph) {
  Projections result;
  std::set<unsigned> call_ids, field_ids;
  auto vertices = graph.vertices();
  std::sort(vertices.begin(), vertices.end());
  for (Vertex v : vertices) {
    State p = result.calls.addControl();
    result.fields.addControl();
    result.controls.emplace(v, p);
  }
  for (const auto &e : graph.edges()) {
    switch (e.label.kind) {
    case LabelKind::OpenParenthesis: case LabelKind::CloseParenthesis:
      call_ids.insert(e.label.id); break;
    case LabelKind::OpenBracket: case LabelKind::CloseBracket:
      field_ids.insert(e.label.id); break;
    case LabelKind::Neutral: break;
    default: throw std::invalid_argument("invalid SPDS graph label kind");
    }
  }
  auto build = [&](PushdownSystem<BooleanSemiring> &system,
                   const std::set<unsigned> &ids, bool call) {
    std::vector<Symbol> alphabet{0};
    for (unsigned id : ids) alphabet.push_back(symbol(id));
    for (const auto &e : graph.edges()) {
      const State from = result.controls.at(e.source), to = result.controls.at(e.target);
      const auto open = call ? LabelKind::OpenParenthesis : LabelKind::OpenBracket;
      const auto close = call ? LabelKind::CloseParenthesis : LabelKind::CloseBracket;
      if (e.label.kind == close) system.addRule(from, symbol(e.label.id), to, {});
      else
        for (Symbol top : alphabet)
          if (e.label.kind == open) system.addRule(from, top, to, {symbol(e.label.id), top});
          else system.addRule(from, top, to, {top});
    }
  };
  build(result.calls, call_ids, true);
  build(result.fields, field_ids, false);
  return result;
}
Automaton<BooleanSemiring> saturate(const PushdownSystem<BooleanSemiring> &system,
                                    State anchor, Direction direction, Limits limits) {
  auto seed = RegularSet::singleton(system.controls(), {anchor, {0}});
  return direction == Direction::Post ? postStar(system, seed, limits)
                                      : preStar(system, seed, limits);
}
void accumulate(Statistics &to, const Statistics &from) {
  to.states += from.states; to.transitions += from.transitions;
  to.updates += from.updates; to.processed += from.processed; to.rules += from.rules;
}
} // namespace

QueryResult::QueryResult(Vertex anchor, Direction direction, Options options,
                         std::map<Vertex, State> controls,
                         Automaton<BooleanSemiring> calls,
                         Automaton<BooleanSemiring> fields)
    : anchor_(anchor), direction_(direction), options_(options),
      controls_(std::move(controls)), calls_(std::move(calls)), fields_(std::move(fields)) {}
bool QueryResult::reachable(const Automaton<BooleanSemiring> &a, Vertex v,
                            StackAcceptance acceptance) const {
  auto p = controls_.find(v);
  if (p == controls_.end()) return false;
  return acceptance == StackAcceptance::Empty ? a.accepts(p->second, {0})
                                             : a.acceptsPrefix(p->second, {});
}
bool QueryResult::parenthesisReachable(Vertex v) const {
  return reachable(calls_, v, options_.parentheses);
}
bool QueryResult::bracketReachable(Vertex v) const {
  return reachable(fields_, v, options_.brackets);
}
bool QueryResult::mayReach(Vertex v) const {
  return parenthesisReachable(v) && bracketReachable(v);
}
bool QueryResult::mayAccept(Vertex v, const std::vector<unsigned> &parentheses,
                            const std::vector<unsigned> &brackets) const {
  auto p = controls_.find(v);
  return p != controls_.end() && calls_.accepts(p->second, stackWord(parentheses)) &&
      fields_.accepts(p->second, stackWord(brackets));
}
QueryResult Solver::query(const Graph &graph, Vertex anchor, Direction direction) const {
  if (!graph.containsVertex(anchor)) throw std::invalid_argument("SPDS query vertex is not in graph");
  auto systems = project(graph);
  State p = systems.controls.at(anchor);
  auto calls = saturate(systems.calls, p, direction, options_.limits);
  auto fields = saturate(systems.fields, p, direction, options_.limits);
  return QueryResult(anchor, direction, options_, std::move(systems.controls),
                     std::move(calls), std::move(fields));
}
QueryResult Solver::analyzeFrom(const Graph &graph, Vertex source) const {
  return query(graph, source, Direction::Post);
}
QueryResult Solver::analyzeTo(const Graph &graph, Vertex target) const {
  return query(graph, target, Direction::Pre);
}
Result Solver::analyze(const Graph &graph) const {
  Result result;
  auto systems = project(graph);
  for (const auto &source : systems.controls) {
    auto calls = saturate(systems.calls, source.second, Direction::Post, options_.limits);
    auto fields = saturate(systems.fields, source.second, Direction::Post, options_.limits);
    QueryResult query(source.first, Direction::Post, options_, systems.controls,
                      std::move(calls), std::move(fields));
    accumulate(result.statistics, query.callAutomaton().statistics());
    accumulate(result.statistics, query.fieldAutomaton().statistics());
    for (const auto &target : systems.controls) {
      const Pair pair{source.first, target.first};
      bool call = query.parenthesisReachable(target.first);
      bool field = query.bracketReachable(target.first);
      if (call) result.parenthesis_pairs.insert(pair);
      if (field) result.bracket_pairs.insert(pair);
      if (call && field) result.upper_bound.insert(pair);
    }
  }
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::spds

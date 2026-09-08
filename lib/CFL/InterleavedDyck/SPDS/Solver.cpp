#include "CFL/InterleavedDyck/SPDS/Solver.h"

namespace lotus::cfl::interleaved_dyck::spds {
namespace {

// 0 is the protected bottom; uint64_t keeps UINT_MAX+1 representable.
Symbol symbol(unsigned id) { return static_cast<Symbol>(id) + 1; }
std::vector<Symbol> stackWord(const std::vector<unsigned> &stack) {
  std::vector<Symbol> result;
  result.reserve(stack.size() + 1);
  for (unsigned id : stack)
    result.push_back(symbol(id));
  result.push_back(0);
  return result;
}
struct Projections {
  std::map<Vertex, State> controls;
  PushdownSystem<BooleanSemiring> calls, fields;
};
Projections project(const Graph &graph) {
  Projections result;
  auto vertices = graph.vertices();
  std::sort(vertices.begin(), vertices.end());
  for (Vertex v : vertices) {
    State p = result.calls.addControl();
    result.fields.addControl();
    result.controls.emplace(v, p);
  }
  auto build = [&](PushdownSystem<BooleanSemiring> &system, bool call) {
    for (const auto &e : graph.edges()) {
      const State from = result.controls.at(e.source),
                  to = result.controls.at(e.target);
      const auto open =
          call ? LabelKind::OpenParenthesis : LabelKind::OpenBracket;
      const auto close =
          call ? LabelKind::CloseParenthesis : LabelKind::CloseBracket;
      if (e.label.kind == close)
        system.addRule(from, symbol(e.label.id), to, {});
      else if (e.label.kind == open)
        system.addPushRule(from, to, symbol(e.label.id));
      else if (e.label.kind == LabelKind::OpenParenthesis ||
               e.label.kind == LabelKind::CloseParenthesis ||
               e.label.kind == LabelKind::OpenBracket ||
               e.label.kind == LabelKind::CloseBracket ||
               e.label.kind == LabelKind::Neutral)
        system.addPreserveRule(from, to);
      else
        throw std::invalid_argument("invalid SPDS graph label kind");
    }
  };
  build(result.calls, true);
  build(result.fields, false);
  return result;
}
Automaton<BooleanSemiring>
saturate(const PushdownSystem<BooleanSemiring> &system, State anchor,
         Direction direction, Limits limits) {
  auto seed = RegularSet::singleton(system.controls(), {anchor, {0}});
  return direction == Direction::Post ? postStar(system, seed, limits)
                                      : preStar(system, seed, limits);
}
void accumulate(Statistics &to, const Statistics &from) {
  to.states += from.states;
  to.transitions += from.transitions;
  to.updates += from.updates;
  to.processed += from.processed;
  to.rules += from.rules;
}
} // namespace

QueryResult::QueryResult(
    Vertex anchor, Direction direction, Options options,
    std::shared_ptr<const std::map<Vertex, State>> controls,
    Automaton<BooleanSemiring> calls, Automaton<BooleanSemiring> fields)
    : anchor_(anchor), direction_(direction), options_(options),
      controls_(std::move(controls)), calls_(std::move(calls)),
      fields_(std::move(fields)) {}
bool QueryResult::reachable(const Automaton<BooleanSemiring> &a, Vertex v,
                            StackAcceptance acceptance) const {
  auto p = controls_->find(v);
  if (p == controls_->end())
    return false;
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
  auto p = controls_->find(v);
  return p != controls_->end() &&
         calls_.accepts(p->second, stackWord(parentheses)) &&
         fields_.accepts(p->second, stackWord(brackets));
}
PreparedAnalysis::PreparedAnalysis(Options options,
                                   std::map<Vertex, State> controls,
                                   PushdownSystem<BooleanSemiring> calls,
                                   PushdownSystem<BooleanSemiring> fields)
    : options_(options),
      controls_(
          std::make_shared<const std::map<Vertex, State>>(std::move(controls))),
      calls_(std::move(calls)), fields_(std::move(fields)) {}
QueryResult PreparedAnalysis::query(Vertex anchor, Direction direction) const {
  auto found = controls_->find(anchor);
  if (found == controls_->end())
    throw std::invalid_argument("SPDS query vertex is not in graph");
  auto calls = saturate(calls_, found->second, direction, options_.limits);
  auto fields = saturate(fields_, found->second, direction, options_.limits);
  return QueryResult(anchor, direction, options_, controls_, std::move(calls),
                     std::move(fields));
}
QueryResult PreparedAnalysis::queryFrom(Vertex source) const {
  return query(source, Direction::Post);
}
QueryResult PreparedAnalysis::queryTo(Vertex target) const {
  return query(target, Direction::Pre);
}
Result PreparedAnalysis::analyzeAll() const {
  Result result;
  for (const auto &source : *controls_) {
    auto query = queryFrom(source.first);
    accumulate(result.statistics, query.callAutomaton().statistics());
    accumulate(result.statistics, query.fieldAutomaton().statistics());
    const auto call_weights =
        options_.parentheses == StackAcceptance::Empty
            ? query.callAutomaton().controlWeights({0})
            : query.callAutomaton().controlWeightsWithPrefix({});
    const auto field_weights =
        options_.brackets == StackAcceptance::Empty
            ? query.fieldAutomaton().controlWeights({0})
            : query.fieldAutomaton().controlWeightsWithPrefix({});
    for (const auto &target : *controls_) {
      const Pair pair{source.first, target.first};
      const bool call = call_weights[target.second];
      const bool field = field_weights[target.second];
      if (call)
        result.parenthesis_pairs.insert(pair);
      if (field)
        result.bracket_pairs.insert(pair);
      if (call && field)
        result.upper_bound.insert(pair);
    }
  }
  return result;
}
Result PreparedAnalysis::analyzeDemands(const std::vector<Pair> &demands,
                                        DemandDirection direction) const {
  std::map<Vertex, std::vector<Vertex>> by_source, by_target;
  std::set<Pair, bool (*)(const Pair &, const Pair &)> unique(
      [](const Pair &a, const Pair &b) {
        return std::tie(a.source, a.target) < std::tie(b.source, b.target);
      });
  for (const Pair &pair : demands) {
    if (!controls_->count(pair.source) || !controls_->count(pair.target))
      throw std::invalid_argument("SPDS demand vertex is not in graph");
    if (unique.insert(pair).second) {
      by_source[pair.source].push_back(pair.target);
      by_target[pair.target].push_back(pair.source);
    }
  }
  const bool use_post = direction == DemandDirection::Post ||
                        (direction == DemandDirection::Auto &&
                         by_source.size() <= by_target.size());
  Result result;
  auto record = [&](const QueryResult &query, Vertex source, Vertex target,
                    Vertex queried) {
    const Pair pair{source, target};
    const bool call = query.parenthesisReachable(queried);
    const bool field = query.bracketReachable(queried);
    if (call)
      result.parenthesis_pairs.insert(pair);
    if (field)
      result.bracket_pairs.insert(pair);
    if (call && field)
      result.upper_bound.insert(pair);
  };
  if (use_post) {
    for (const auto &entry : by_source) {
      auto query = queryFrom(entry.first);
      accumulate(result.statistics, query.callAutomaton().statistics());
      accumulate(result.statistics, query.fieldAutomaton().statistics());
      if (entry.second.size() < 8) {
        for (Vertex target : entry.second)
          record(query, entry.first, target, target);
      } else {
        const auto calls =
            options_.parentheses == StackAcceptance::Empty
                ? query.callAutomaton().controlWeights({0})
                : query.callAutomaton().controlWeightsWithPrefix({});
        const auto fields =
            options_.brackets == StackAcceptance::Empty
                ? query.fieldAutomaton().controlWeights({0})
                : query.fieldAutomaton().controlWeightsWithPrefix({});
        for (Vertex target : entry.second) {
          const Pair pair{entry.first, target};
          const State state = controls_->at(target);
          if (calls[state])
            result.parenthesis_pairs.insert(pair);
          if (fields[state])
            result.bracket_pairs.insert(pair);
          if (calls[state] && fields[state])
            result.upper_bound.insert(pair);
        }
      }
    }
  } else {
    for (const auto &entry : by_target) {
      auto query = queryTo(entry.first);
      accumulate(result.statistics, query.callAutomaton().statistics());
      accumulate(result.statistics, query.fieldAutomaton().statistics());
      if (entry.second.size() < 8) {
        for (Vertex source : entry.second)
          record(query, source, entry.first, source);
      } else {
        const auto calls =
            options_.parentheses == StackAcceptance::Empty
                ? query.callAutomaton().controlWeights({0})
                : query.callAutomaton().controlWeightsWithPrefix({});
        const auto fields =
            options_.brackets == StackAcceptance::Empty
                ? query.fieldAutomaton().controlWeights({0})
                : query.fieldAutomaton().controlWeightsWithPrefix({});
        for (Vertex source : entry.second) {
          const Pair pair{source, entry.first};
          const State state = controls_->at(source);
          if (calls[state])
            result.parenthesis_pairs.insert(pair);
          if (fields[state])
            result.bracket_pairs.insert(pair);
          if (calls[state] && fields[state])
            result.upper_bound.insert(pair);
        }
      }
    }
  }
  return result;
}
PreparedAnalysis Solver::prepare(const Graph &graph) const {
  auto systems = project(graph);
  return PreparedAnalysis(options_, std::move(systems.controls),
                          std::move(systems.calls), std::move(systems.fields));
}

} // namespace lotus::cfl::interleaved_dyck::spds

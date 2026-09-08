#include "CFL/InterleavedDyck/AffineSPDS/Solver.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace lotus::cfl::interleaved_dyck::affine {
HistoryComparison::HistoryComparison(AffineSpace call_history, AffineSpace field_history)
    : calls_(std::move(call_history)), fields_(std::move(field_history)), verdict_(Verdict::MayReach) {
  if (calls_.dimension() != fields_.dimension()) throw std::invalid_argument("history dimension mismatch");
  if (!spdsMayReach()) verdict_ = Verdict::ProjectionRejected;
  else {
    certificate_ = separate(calls_, fields_);
    if (certificate_) verdict_ = Verdict::AffineSeparated;
  }
}
bool HistoryComparison::independentMayReach(const HistoryObserver &observer) const {
  if (observer.dimension() != calls_.dimension()) throw std::invalid_argument("observer/history mismatch");
  if (!spdsMayReach()) return false;
  for (const auto &block : observer.blocks())
    if (!calls_.block(block.first, block.second).intersects(fields_.block(block.first, block.second)))
      return false;
  return true;
}
HistoryComparison synchronize(const spds::Automaton<AffineSemiring> &calls,
                              const spds::Configuration &call_query,
                              const spds::Automaton<AffineSemiring> &fields,
                              const spds::Configuration &field_query) {
  if (calls.direction() != fields.direction()) throw std::invalid_argument("mixed post/pre synchronization");
  return HistoryComparison(calls.weight(call_query.control, call_query.stack),
                           fields.weight(field_query.control, field_query.stack));
}
namespace {
using spds::Symbol;
using spds::State;
using System = spds::PushdownSystem<AffineSemiring>;
using Automaton = spds::Automaton<AffineSemiring>;
Symbol symbol(unsigned id) { return static_cast<Symbol>(id) + 1; }
std::vector<Symbol> word(const std::vector<unsigned> &ids) {
  std::vector<Symbol> result;
  for (unsigned id : ids) result.push_back(symbol(id));
  result.push_back(0); return result;
}
struct Projections {
  explicit Projections(std::size_t dimension) : calls(AffineSemiring(dimension)), fields(AffineSemiring(dimension)) {}
  std::map<Vertex, State> controls;
  System calls, fields;
};
Projections project(const Graph &graph, const HistoryObserver &observer) {
  Projections result(observer.dimension()); std::set<unsigned> call_ids, field_ids;
  auto vertices = graph.vertices(); std::sort(vertices.begin(), vertices.end());
  for (Vertex v : vertices) {
    const auto p = result.calls.addControl(); result.fields.addControl(); result.controls.emplace(v, p);
  }
  auto edges = graph.edges(); std::sort(edges.begin(), edges.end(), EdgeLess{});
  for (const auto &edge : edges) {
    switch (edge.label.kind) {
    case LabelKind::OpenParenthesis: case LabelKind::CloseParenthesis: call_ids.insert(edge.label.id); break;
    case LabelKind::OpenBracket: case LabelKind::CloseBracket: field_ids.insert(edge.label.id); break;
    case LabelKind::Neutral: break;
    default: throw std::invalid_argument("invalid AffineSPDS graph label kind");
    }
  }
  auto build = [&](System &system, const std::set<unsigned> &ids, bool calls) {
    std::vector<Symbol> alphabet{0};
    for (auto id : ids) alphabet.push_back(symbol(id));
    const auto open = calls ? LabelKind::OpenParenthesis : LabelKind::OpenBracket;
    const auto close = calls ? LabelKind::CloseParenthesis : LabelKind::CloseBracket;
    for (const auto &edge : edges) {
      const auto from = result.controls.at(edge.source), to = result.controls.at(edge.target);
      // Even a stack-neutral original edge contributes exactly ONE event.
      const auto weight = system.domain().lift(observer.matrix(edge));
      if (edge.label.kind == close) system.addRule(from, symbol(edge.label.id), to, {}, weight);
      else for (Symbol top : alphabet) {
        if (edge.label.kind == open) system.addRule(from, top, to, {symbol(edge.label.id), top}, weight);
        else system.addRule(from, top, to, {top}, weight);
      }
    }
  };
  build(result.calls, call_ids, true); build(result.fields, field_ids, false); return result;
}
Automaton saturate(const System &system, State anchor, spds::Direction direction, spds::Limits limits) {
  auto seed = spds::RegularSet::singleton(system.controls(), {anchor, {0}});
  return direction == spds::Direction::Post ? spds::postStar(system, seed, limits) : spds::preStar(system, seed, limits);
}
void add(spds::Statistics &a, const spds::Statistics &b) {
  a.states += b.states; a.transitions += b.transitions; a.updates += b.updates;
  a.processed += b.processed; a.rules += b.rules;
}
}
QueryResult::QueryResult(Vertex anchor, spds::Direction direction, Options options,
                         std::map<Vertex, State> controls, std::shared_ptr<const HistoryObserver> observer,
                         Automaton calls, Automaton fields)
    : anchor_(anchor), direction_(direction), options_(options), controls_(std::move(controls)),
      observer_(std::move(observer)), calls_(std::move(calls)), fields_(std::move(fields)) {
  statistics_.matrix_dimension = observer_->dimension();
  for (const auto *automaton : {&calls_, &fields_}) {
    add(statistics_.saturation, automaton->statistics());
    for (const auto &entry : automaton->transitions())
      statistics_.maximum_affine_rank = std::max(statistics_.maximum_affine_rank, entry.second.rank());
  }
}
AffineSpace QueryResult::history(const Automaton &automaton, Vertex vertex, spds::StackAcceptance acceptance) const {
  const auto found = controls_.find(vertex);
  if (found == controls_.end()) return automaton.domain().zero();
  return acceptance == spds::StackAcceptance::Empty ? automaton.weight(found->second, {0})
                                                   : automaton.weightWithPrefix(found->second, {});
}
const HistoryComparison &QueryResult::compare(Vertex vertex) const {
  auto found = cache_.find(vertex);
  if (found == cache_.end())
    found = cache_.emplace(vertex, HistoryComparison(history(calls_, vertex, options_.parentheses),
                                                    history(fields_, vertex, options_.brackets))).first;
  return found->second;
}
HistoryComparison QueryResult::compareStacks(Vertex vertex, const std::vector<unsigned> &calls,
                                             const std::vector<unsigned> &fields) const {
  auto found = controls_.find(vertex);
  if (found == controls_.end()) return HistoryComparison(calls_.domain().zero(), fields_.domain().zero());
  return synchronize(calls_, {found->second, word(calls)}, fields_, {found->second, word(fields)});
}
void Solver::validate(const Graph &graph, const HistoryObserver &observer) const {
  if (options_.max_matrix_dimension && observer.dimension() > options_.max_matrix_dimension)
    throw spds::ResourceLimit("AffineSPDS matrix-dimension limit exceeded");
  observer.validate(graph);
}
QueryResult Solver::query(const Graph &graph, Vertex anchor, spds::Direction direction,
                          const HistoryObserver &observer) const {
  if (!graph.containsVertex(anchor)) throw std::invalid_argument("AffineSPDS query vertex is not in graph");
  validate(graph, observer);
  auto projected = project(graph, observer); const auto state = projected.controls.at(anchor);
  auto calls = saturate(projected.calls, state, direction, options_.limits);
  auto fields = saturate(projected.fields, state, direction, options_.limits);
  return QueryResult(anchor, direction, options_, std::move(projected.controls),
                     std::make_shared<HistoryObserver>(observer), std::move(calls), std::move(fields));
}
QueryResult Solver::analyzeFrom(const Graph &g, Vertex s) const { return analyzeFrom(g, s, HistoryObserver::automatic(g, options_.observer)); }
QueryResult Solver::analyzeFrom(const Graph &g, Vertex s, const HistoryObserver &o) const { return query(g, s, spds::Direction::Post, o); }
QueryResult Solver::analyzeTo(const Graph &g, Vertex t) const { return analyzeTo(g, t, HistoryObserver::automatic(g, options_.observer)); }
QueryResult Solver::analyzeTo(const Graph &g, Vertex t, const HistoryObserver &o) const { return query(g, t, spds::Direction::Pre, o); }
Result Solver::analyze(const Graph &g) const { return analyze(g, HistoryObserver::automatic(g, options_.observer)); }
Result Solver::analyze(const Graph &graph, const HistoryObserver &observer) const {
  validate(graph, observer); Result result; result.statistics.matrix_dimension = observer.dimension();
  auto projected = project(graph, observer); auto shared = std::make_shared<HistoryObserver>(observer);
  for (const auto &source : projected.controls) {
    auto calls = saturate(projected.calls, source.second, spds::Direction::Post, options_.limits);
    auto fields = saturate(projected.fields, source.second, spds::Direction::Post, options_.limits);
    QueryResult query(source.first, spds::Direction::Post, options_, projected.controls, shared, std::move(calls), std::move(fields));
    add(result.statistics.saturation, query.statistics().saturation);
    result.statistics.maximum_affine_rank = std::max(result.statistics.maximum_affine_rank, query.statistics().maximum_affine_rank);
    for (const auto &target : projected.controls) {
      const Pair pair{source.first, target.first};
      if (query.spdsMayReach(target.first)) result.spds_upper_bound.insert(pair);
      if (query.independentMayReach(target.first)) result.independent_upper_bound.insert(pair);
      if (query.mayReach(target.first)) result.upper_bound.insert(pair);
    }
  }
  return result;
}
} // namespace lotus::cfl::interleaved_dyck::affine

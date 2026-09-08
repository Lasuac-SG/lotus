#include "CFL/InterleavedDyck/AffineSPDS/Solver.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

namespace lotus::cfl::interleaved_dyck::affine {
HistoryComparison::HistoryComparison(AffineSpace call_history,
                                     AffineSpace field_history)
    : calls_(std::move(call_history)), fields_(std::move(field_history)) {
  if (calls_.dimension() != fields_.dimension())
    throw std::invalid_argument("history dimension mismatch");
}
Verdict HistoryComparison::verdict() const {
  if (!verdict_)
    verdict_ = !spdsMayReach()              ? Verdict::ProjectionRejected
               : calls_.intersects(fields_) ? Verdict::MayReach
                                            : Verdict::AffineSeparated;
  return *verdict_;
}
bool HistoryComparison::mayReach() const {
  return verdict() == Verdict::MayReach;
}
const std::optional<SeparationCertificate> &
HistoryComparison::certificate() const {
  if (!certificate_)
    certificate_ = verdict() == Verdict::AffineSeparated
                       ? separate(calls_, fields_)
                       : std::optional<SeparationCertificate>{};
  return *certificate_;
}
bool HistoryComparison::independentMayReach(
    const HistoryObserver &observer) const {
  if (observer.dimension() != calls_.dimension())
    throw std::invalid_argument("observer/history mismatch");
  if (!spdsMayReach())
    return false;
  for (const auto &block : observer.blocks())
    if (!calls_.block(block.first, block.second)
             .intersects(fields_.block(block.first, block.second)))
      return false;
  return true;
}
HistoryComparison synchronize(const spds::Automaton<AffineSemiring> &calls,
                              const spds::Configuration &call_query,
                              const spds::Automaton<AffineSemiring> &fields,
                              const spds::Configuration &field_query) {
  if (calls.direction() != fields.direction())
    throw std::invalid_argument("mixed post/pre synchronization");
  return HistoryComparison(
      calls.weight(call_query.control, call_query.stack),
      fields.weight(field_query.control, field_query.stack));
}
namespace {
using spds::State;
using spds::Symbol;
using System = spds::PushdownSystem<AffineSemiring>;
using Automaton = spds::Automaton<AffineSemiring>;
Symbol symbol(unsigned id) { return static_cast<Symbol>(id) + 1; }
std::vector<Symbol> word(const std::vector<unsigned> &ids) {
  std::vector<Symbol> result;
  for (unsigned id : ids)
    result.push_back(symbol(id));
  result.push_back(0);
  return result;
}
struct Projections {
  explicit Projections(std::size_t dimension)
      : calls(AffineSemiring(dimension)), fields(AffineSemiring(dimension)) {}
  std::map<Vertex, State> controls;
  System calls, fields;
};
Projections project(const Graph &graph, const HistoryObserver &observer) {
  Projections result(observer.dimension());
  auto vertices = graph.vertices();
  std::sort(vertices.begin(), vertices.end());
  for (Vertex v : vertices) {
    const auto p = result.calls.addControl();
    result.fields.addControl();
    result.controls.emplace(v, p);
  }
  auto edges = graph.edges();
  std::sort(edges.begin(), edges.end(), EdgeLess{});
  auto build = [&](System &system, bool calls) {
    const auto open =
        calls ? LabelKind::OpenParenthesis : LabelKind::OpenBracket;
    const auto close =
        calls ? LabelKind::CloseParenthesis : LabelKind::CloseBracket;
    for (const auto &edge : edges) {
      const auto from = result.controls.at(edge.source),
                 to = result.controls.at(edge.target);
      // Even a stack-neutral original edge contributes exactly ONE event.
      const auto weight = system.domain().lift(observer.matrix(edge));
      if (edge.label.kind == close)
        system.addRule(from, symbol(edge.label.id), to, {}, weight);
      else if (edge.label.kind == open)
        system.addPushRule(from, to, symbol(edge.label.id), weight);
      else if (edge.label.kind == LabelKind::OpenParenthesis ||
               edge.label.kind == LabelKind::CloseParenthesis ||
               edge.label.kind == LabelKind::OpenBracket ||
               edge.label.kind == LabelKind::CloseBracket ||
               edge.label.kind == LabelKind::Neutral)
        system.addPreserveRule(from, to, weight);
      else
        throw std::invalid_argument("invalid AffineSPDS graph label kind");
    }
  };
  build(result.calls, true);
  build(result.fields, false);
  return result;
}
Automaton saturate(const System &system, State anchor,
                   spds::Direction direction, spds::Limits limits) {
  auto seed = spds::RegularSet::singleton(system.controls(), {anchor, {0}});
  return direction == spds::Direction::Post
             ? spds::postStar(system, seed, limits)
             : spds::preStar(system, seed, limits);
}
void add(spds::Statistics &a, const spds::Statistics &b) {
  a.states += b.states;
  a.transitions += b.transitions;
  a.updates += b.updates;
  a.processed += b.processed;
  a.rules += b.rules;
  a.setup_microseconds += b.setup_microseconds;
  a.saturation_microseconds += b.saturation_microseconds;
  a.readout_microseconds += b.readout_microseconds;
  a.projection_microseconds += b.projection_microseconds;
}
} // namespace
QueryResult::QueryResult(
    Vertex anchor, spds::Direction direction, Options options,
    std::shared_ptr<const std::map<Vertex, State>> controls,
    std::shared_ptr<const HistoryObserver> observer, Automaton calls,
    Automaton fields)
    : anchor_(anchor), direction_(direction), options_(options),
      controls_(std::move(controls)), observer_(std::move(observer)),
      calls_(std::move(calls)), fields_(std::move(fields)) {
  statistics_.matrix_dimension = observer_->dimension();
  for (const auto *automaton : {&calls_, &fields_}) {
    add(statistics_.saturation, automaton->statistics());
    for (const auto &entry : automaton->transitions())
      statistics_.maximum_affine_rank =
          std::max(statistics_.maximum_affine_rank, entry.weight.rank());
  }
}
Statistics QueryResult::statistics() const {
  Statistics result = statistics_;
  result.saturation.readout_microseconds =
      calls_.statistics().readout_microseconds +
      fields_.statistics().readout_microseconds;
  return result;
}
AffineSpace QueryResult::history(const Automaton &automaton, Vertex vertex,
                                 spds::StackAcceptance acceptance) const {
  const auto found = controls_->find(vertex);
  if (found == controls_->end())
    return automaton.domain().zero();
  return acceptance == spds::StackAcceptance::Empty
             ? automaton.weight(found->second, {0})
             : automaton.weightWithPrefix(found->second, {});
}
const HistoryComparison &QueryResult::compare(Vertex vertex) const {
  auto found = cache_.find(vertex);
  if (found == cache_.end())
    found =
        cache_
            .emplace(vertex, HistoryComparison(
                                 history(calls_, vertex, options_.parentheses),
                                 history(fields_, vertex, options_.brackets)))
            .first;
  return found->second;
}
HistoryComparison
QueryResult::compareStacks(Vertex vertex, const std::vector<unsigned> &calls,
                           const std::vector<unsigned> &fields) const {
  auto found = controls_->find(vertex);
  if (found == controls_->end())
    return HistoryComparison(calls_.domain().zero(), fields_.domain().zero());
  return synchronize(calls_, {found->second, word(calls)}, fields_,
                     {found->second, word(fields)});
}
void Solver::validate(const Graph &graph,
                      const HistoryObserver &observer) const {
  if (options_.max_matrix_dimension &&
      observer.dimension() > options_.max_matrix_dimension)
    throw spds::ResourceLimit("AffineSPDS matrix-dimension limit exceeded");
  observer.validate(graph);
}
PreparedAnalysis::PreparedAnalysis(
    Options options, const Graph &graph, std::map<Vertex, State> controls,
    std::shared_ptr<const HistoryObserver> observer, System calls,
    System fields, std::optional<spds::PreparedAnalysis> boolean)
    : options_(options),
      controls_(
          std::make_shared<const std::map<Vertex, State>>(std::move(controls))),
      observer_(std::move(observer)), calls_(std::move(calls)),
      fields_(std::move(fields)), boolean_(std::move(boolean)),
      vertices_(controls_->size()), edges_(graph.edges()),
      successors_(controls_->size()), predecessors_(controls_->size()) {
  for (const auto &entry : *controls_)
    vertices_[entry.second] = entry.first;
  for (const auto &edge : edges_) {
    const State from = controls_->at(edge.source);
    const State to = controls_->at(edge.target);
    successors_[from].push_back(to);
    predecessors_[to].push_back(from);
  }
}
std::optional<Graph>
PreparedAnalysis::relevantGraph(Vertex anchor,
                                spds::Direction direction) const {
  const State start = controls_->at(anchor);
  const auto &adjacency =
      direction == spds::Direction::Post ? successors_ : predecessors_;
  std::vector<bool> live(controls_->size(), false);
  std::vector<State> worklist{start};
  live[start] = true;
  for (std::size_t cursor = 0; cursor < worklist.size(); ++cursor)
    for (State next : adjacency[worklist[cursor]])
      if (!live[next]) {
        live[next] = true;
        worklist.push_back(next);
      }
  if (worklist.size() == controls_->size())
    return std::nullopt;
  Graph result;
  for (State state : worklist)
    result.addVertex(vertices_[state]);
  for (const auto &edge : edges_)
    if (live[controls_->at(edge.source)] && live[controls_->at(edge.target)])
      result.addEdge(edge.source, edge.target, edge.label);
  return result;
}
QueryResult PreparedAnalysis::query(Vertex anchor, spds::Direction direction,
                                    bool slice_graph) const {
  auto found = controls_->find(anchor);
  if (found == controls_->end())
    throw std::invalid_argument("AffineSPDS query vertex is not in graph");
  if (slice_graph) {
    const auto slice_started = std::chrono::steady_clock::now();
    if (auto graph = relevantGraph(anchor, direction)) {
      auto systems = project(*graph, *observer_);
      const auto slice_projection_microseconds = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - slice_started)
              .count());
      const State state = systems.controls.at(anchor);
      auto calls = saturate(systems.calls, state, direction, options_.limits);
      auto fields = saturate(systems.fields, state, direction, options_.limits);
      auto controls = std::make_shared<const std::map<Vertex, State>>(
          std::move(systems.controls));
      QueryResult result(anchor, direction, options_, std::move(controls),
                         observer_, std::move(calls), std::move(fields));
      result.statistics_.saturation.projection_microseconds =
          slice_projection_microseconds;
      return result;
    }
  }
  auto calls = saturate(calls_, found->second, direction, options_.limits);
  auto fields = saturate(fields_, found->second, direction, options_.limits);
  QueryResult result(anchor, direction, options_, controls_, observer_,
                     std::move(calls), std::move(fields));
  return result;
}
QueryResult PreparedAnalysis::queryFrom(Vertex source) const {
  return query(source, spds::Direction::Post);
}
QueryResult PreparedAnalysis::queryTo(Vertex target) const {
  return query(target, spds::Direction::Pre);
}
namespace {
std::vector<AffineSemiring::Weight>
endpointWeights(const Automaton &automaton, spds::StackAcceptance acceptance) {
  return acceptance == spds::StackAcceptance::Empty
             ? automaton.controlWeights({0})
             : automaton.controlWeightsWithPrefix({});
}
bool selected(const HistoryComparison &comparison,
              const HistoryObserver &observer, ComparisonMode mode) {
  switch (mode) {
  case ComparisonMode::Joint:
    return comparison.mayReach();
  case ComparisonMode::Independent:
    return comparison.independentMayReach(observer);
  case ComparisonMode::Projection:
    return comparison.spdsMayReach();
  }
  throw std::invalid_argument("invalid AffineSPDS comparison mode");
}
} // namespace
Result PreparedAnalysis::analyzeFrom(Vertex source, ComparisonMode mode) const {
  Result result;
  result.mode = mode;
  result.statistics.matrix_dimension = observer_->dimension();
  result.statistics.saturation.projection_microseconds =
      projection_microseconds_;
  if (boolean_) {
    auto baseline = boolean_->analyzeFrom(source);
    result.pairs = std::move(baseline.upper_bound);
    result.statistics.saturation = baseline.statistics;
    result.statistics.saturation.projection_microseconds =
        projection_microseconds_;
    return result;
  }
  auto query = queryFrom(source);
  auto calls = endpointWeights(query.callAutomaton(), options_.parentheses);
  auto fields = endpointWeights(query.fieldAutomaton(), options_.brackets);
  result.statistics = query.statistics();
  result.statistics.saturation.projection_microseconds +=
      projection_microseconds_;
  for (const auto &target : *controls_) {
    const auto found = query.controls_->find(target.first);
    if (found == query.controls_->end())
      continue;
    HistoryComparison comparison(std::move(calls[found->second]),
                                 std::move(fields[found->second]));
    if (selected(comparison, *observer_, mode))
      result.pairs.insert({source, target.first});
  }
  return result;
}
Result PreparedAnalysis::analyzeTo(Vertex target, ComparisonMode mode) const {
  Result result;
  result.mode = mode;
  result.statistics.matrix_dimension = observer_->dimension();
  result.statistics.saturation.projection_microseconds =
      projection_microseconds_;
  if (boolean_) {
    auto baseline = boolean_->analyzeTo(target);
    result.pairs = std::move(baseline.upper_bound);
    result.statistics.saturation = baseline.statistics;
    result.statistics.saturation.projection_microseconds =
        projection_microseconds_;
    return result;
  }
  auto query = queryTo(target);
  auto calls = endpointWeights(query.callAutomaton(), options_.parentheses);
  auto fields = endpointWeights(query.fieldAutomaton(), options_.brackets);
  result.statistics = query.statistics();
  result.statistics.saturation.projection_microseconds +=
      projection_microseconds_;
  for (const auto &source : *controls_) {
    const auto found = query.controls_->find(source.first);
    if (found == query.controls_->end())
      continue;
    HistoryComparison comparison(std::move(calls[found->second]),
                                 std::move(fields[found->second]));
    if (selected(comparison, *observer_, mode))
      result.pairs.insert({source.first, target});
  }
  return result;
}
Result PreparedAnalysis::analyzeAll(ComparisonMode mode) const {
  Result result;
  result.mode = mode;
  result.statistics.matrix_dimension = observer_->dimension();
  result.statistics.saturation.projection_microseconds =
      projection_microseconds_;
  if (boolean_) {
    auto baseline = boolean_->analyzeAll();
    result.pairs = std::move(baseline.upper_bound);
    result.statistics.saturation = baseline.statistics;
    result.statistics.saturation.projection_microseconds =
        projection_microseconds_;
    return result;
  }
  for (const auto &source : *controls_) {
    auto query = this->query(source.first, spds::Direction::Post, false);
    auto call_weights =
        endpointWeights(query.callAutomaton(), options_.parentheses);
    auto field_weights =
        endpointWeights(query.fieldAutomaton(), options_.brackets);
    const auto query_stats = query.statistics();
    add(result.statistics.saturation, query_stats.saturation);
    result.statistics.maximum_affine_rank = std::max(
        result.statistics.maximum_affine_rank, query_stats.maximum_affine_rank);
    for (const auto &target : *controls_) {
      const Pair pair{source.first, target.first};
      HistoryComparison comparison(std::move(call_weights[target.second]),
                                   std::move(field_weights[target.second]));
      if (selected(comparison, *observer_, mode))
        result.pairs.insert(pair);
    }
  }
  return result;
}
Result PreparedAnalysis::analyzeDemands(const std::vector<Pair> &demands,
                                        ComparisonMode mode,
                                        spds::DemandDirection direction) const {
  std::map<Vertex, std::vector<Vertex>> by_source, by_target;
  PairSet unique;
  for (const Pair &pair : demands) {
    if (!controls_->count(pair.source) || !controls_->count(pair.target))
      throw std::invalid_argument("AffineSPDS demand vertex is not in graph");
    if (unique.insert(pair).second) {
      by_source[pair.source].push_back(pair.target);
      by_target[pair.target].push_back(pair.source);
    }
  }
  Result result;
  result.mode = mode;
  result.statistics.matrix_dimension = observer_->dimension();
  result.statistics.saturation.projection_microseconds =
      projection_microseconds_;
  if (boolean_) {
    auto baseline = boolean_->analyzeDemands(demands, direction);
    result.pairs = std::move(baseline.upper_bound);
    result.statistics.saturation = baseline.statistics;
    result.statistics.saturation.projection_microseconds =
        projection_microseconds_;
    return result;
  }
  const bool use_post = direction == spds::DemandDirection::Post ||
                        (direction == spds::DemandDirection::Auto &&
                         by_source.size() <= by_target.size());
  auto merge_stats = [&](const QueryResult &query) {
    add(result.statistics.saturation, query.statistics().saturation);
    result.statistics.maximum_affine_rank =
        std::max(result.statistics.maximum_affine_rank,
                 query.statistics().maximum_affine_rank);
  };
  if (use_post) {
    for (const auto &entry : by_source) {
      auto query = queryFrom(entry.first);
      auto calls = endpointWeights(query.callAutomaton(), options_.parentheses);
      auto fields = endpointWeights(query.fieldAutomaton(), options_.brackets);
      merge_stats(query);
      for (Vertex target : entry.second) {
        const auto found = query.controls_->find(target);
        if (found == query.controls_->end())
          continue;
        const State state = found->second;
        HistoryComparison comparison(std::move(calls[state]),
                                     std::move(fields[state]));
        if (selected(comparison, *observer_, mode))
          result.pairs.insert({entry.first, target});
      }
    }
  } else {
    for (const auto &entry : by_target) {
      auto query = queryTo(entry.first);
      auto calls = endpointWeights(query.callAutomaton(), options_.parentheses);
      auto fields = endpointWeights(query.fieldAutomaton(), options_.brackets);
      merge_stats(query);
      for (Vertex source : entry.second) {
        const auto found = query.controls_->find(source);
        if (found == query.controls_->end())
          continue;
        const State state = found->second;
        HistoryComparison comparison(std::move(calls[state]),
                                     std::move(fields[state]));
        if (selected(comparison, *observer_, mode))
          result.pairs.insert({source, entry.first});
      }
    }
  }
  return result;
}
PreparedAnalysis Solver::prepare(const Graph &graph) const {
  return prepare(graph, HistoryObserver::automatic(graph, options_.observer));
}
PreparedAnalysis Solver::prepare(const Graph &graph,
                                 const HistoryObserver &observer) const {
  const auto started = std::chrono::steady_clock::now();
  validate(graph, observer);
  auto projected = project(graph, observer);
  bool identity = true;
  for (const auto &edge : graph.edges())
    if (!observer.matrix(edge).isIdentity()) {
      identity = false;
      break;
    }
  std::optional<spds::PreparedAnalysis> boolean;
  if (identity) {
    spds::Options options;
    options.parentheses = options_.parentheses;
    options.brackets = options_.brackets;
    options.limits = options_.limits;
    boolean.emplace(spds::Solver(options).prepare(graph));
  }
  PreparedAnalysis result(options_, graph, std::move(projected.controls),
                          std::make_shared<const HistoryObserver>(observer),
                          std::move(projected.calls),
                          std::move(projected.fields), std::move(boolean));
  result.projection_microseconds_ = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started)
          .count());
  return result;
}
} // namespace lotus::cfl::interleaved_dyck::affine

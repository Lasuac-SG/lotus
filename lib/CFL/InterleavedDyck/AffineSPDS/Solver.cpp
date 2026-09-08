#include "CFL/InterleavedDyck/AffineSPDS/Solver.h"

#include <algorithm>
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
          std::max(statistics_.maximum_affine_rank, entry.second.rank());
  }
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
    Options options, std::map<Vertex, State> controls,
    std::shared_ptr<const HistoryObserver> observer, System calls,
    System fields, std::optional<spds::PreparedAnalysis> boolean)
    : options_(options),
      controls_(
          std::make_shared<const std::map<Vertex, State>>(std::move(controls))),
      observer_(std::move(observer)), calls_(std::move(calls)),
      fields_(std::move(fields)), boolean_(std::move(boolean)) {}
QueryResult PreparedAnalysis::query(Vertex anchor,
                                    spds::Direction direction) const {
  auto found = controls_->find(anchor);
  if (found == controls_->end())
    throw std::invalid_argument("AffineSPDS query vertex is not in graph");
  auto calls = saturate(calls_, found->second, direction, options_.limits);
  auto fields = saturate(fields_, found->second, direction, options_.limits);
  return QueryResult(anchor, direction, options_, controls_, observer_,
                     std::move(calls), std::move(fields));
}
QueryResult PreparedAnalysis::queryFrom(Vertex source) const {
  return query(source, spds::Direction::Post);
}
QueryResult PreparedAnalysis::queryTo(Vertex target) const {
  return query(target, spds::Direction::Pre);
}
namespace {
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
bool selected(const QueryResult &query, Vertex vertex, ComparisonMode mode) {
  return selected(query.compare(vertex), query.observer(), mode);
}
} // namespace
Result PreparedAnalysis::analyzeAll(ComparisonMode mode) const {
  Result result;
  result.mode = mode;
  result.statistics.matrix_dimension = observer_->dimension();
  if (boolean_) {
    auto baseline = boolean_->analyzeAll();
    result.pairs = std::move(baseline.upper_bound);
    result.statistics.saturation = baseline.statistics;
    return result;
  }
  for (const auto &source : *controls_) {
    auto query = queryFrom(source.first);
    add(result.statistics.saturation, query.statistics().saturation);
    result.statistics.maximum_affine_rank =
        std::max(result.statistics.maximum_affine_rank,
                 query.statistics().maximum_affine_rank);
    auto call_weights =
        options_.parentheses == spds::StackAcceptance::Empty
            ? query.callAutomaton().controlWeights({0})
            : query.callAutomaton().controlWeightsWithPrefix({});
    auto field_weights =
        options_.brackets == spds::StackAcceptance::Empty
            ? query.fieldAutomaton().controlWeights({0})
            : query.fieldAutomaton().controlWeightsWithPrefix({});
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
  const bool use_post = direction == spds::DemandDirection::Post ||
                        (direction == spds::DemandDirection::Auto &&
                         by_source.size() <= by_target.size());
  Result result;
  result.mode = mode;
  result.statistics.matrix_dimension = observer_->dimension();
  if (boolean_) {
    auto baseline = boolean_->analyzeDemands(demands, direction);
    result.pairs = std::move(baseline.upper_bound);
    result.statistics.saturation = baseline.statistics;
    return result;
  }
  auto merge_stats = [&](const QueryResult &query) {
    add(result.statistics.saturation, query.statistics().saturation);
    result.statistics.maximum_affine_rank =
        std::max(result.statistics.maximum_affine_rank,
                 query.statistics().maximum_affine_rank);
  };
  if (use_post) {
    for (const auto &entry : by_source) {
      auto query = queryFrom(entry.first);
      merge_stats(query);
      if (entry.second.size() < 8) {
        for (Vertex target : entry.second)
          if (selected(query, target, mode))
            result.pairs.insert({entry.first, target});
      } else {
        auto calls = options_.parentheses == spds::StackAcceptance::Empty
                         ? query.callAutomaton().controlWeights({0})
                         : query.callAutomaton().controlWeightsWithPrefix({});
        auto fields = options_.brackets == spds::StackAcceptance::Empty
                          ? query.fieldAutomaton().controlWeights({0})
                          : query.fieldAutomaton().controlWeightsWithPrefix({});
        for (Vertex target : entry.second) {
          const State state = controls_->at(target);
          HistoryComparison comparison(std::move(calls[state]),
                                       std::move(fields[state]));
          if (selected(comparison, *observer_, mode))
            result.pairs.insert({entry.first, target});
        }
      }
    }
  } else {
    for (const auto &entry : by_target) {
      auto query = queryTo(entry.first);
      merge_stats(query);
      if (entry.second.size() < 8) {
        for (Vertex source : entry.second)
          if (selected(query, source, mode))
            result.pairs.insert({source, entry.first});
      } else {
        auto calls = options_.parentheses == spds::StackAcceptance::Empty
                         ? query.callAutomaton().controlWeights({0})
                         : query.callAutomaton().controlWeightsWithPrefix({});
        auto fields = options_.brackets == spds::StackAcceptance::Empty
                          ? query.fieldAutomaton().controlWeights({0})
                          : query.fieldAutomaton().controlWeightsWithPrefix({});
        for (Vertex source : entry.second) {
          const State state = controls_->at(source);
          HistoryComparison comparison(std::move(calls[state]),
                                       std::move(fields[state]));
          if (selected(comparison, *observer_, mode))
            result.pairs.insert({source, entry.first});
        }
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
  return PreparedAnalysis(options_, std::move(projected.controls),
                          std::make_shared<const HistoryObserver>(observer),
                          std::move(projected.calls),
                          std::move(projected.fields), std::move(boolean));
}
} // namespace lotus::cfl::interleaved_dyck::affine

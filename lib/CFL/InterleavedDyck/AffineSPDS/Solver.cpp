#include "CFL/InterleavedDyck/AffineSPDS/Solver.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>

namespace lotus::cfl::interleaved_dyck::affine {
namespace {
using Clock = std::chrono::steady_clock;
using spds::State;
using spds::Symbol;
using System = spds::PushdownSystem<AffineSemiring>;
using Automaton = spds::Automaton<AffineSemiring>;
constexpr State Missing = std::numeric_limits<State>::max();
std::uint64_t elapsed(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                               start)
      .count();
}
Symbol symbol(unsigned id) { return static_cast<Symbol>(id) + 1; }
std::vector<Symbol> word(const std::vector<unsigned> &ids) {
  std::vector<Symbol> result;
  result.reserve(ids.size() + 1);
  for (unsigned id : ids)
    result.push_back(symbol(id));
  result.push_back(0);
  return result;
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
void add(Statistics &a, const Statistics &b) {
  add(a.saturation, b.saturation);
  a.slice_cache_hits += b.slice_cache_hits;
  a.compiled_rules += b.compiled_rules;
  a.prepared_weights += b.prepared_weights;
  a.maximum_affine_rank =
      std::max(a.maximum_affine_rank, b.maximum_affine_rank);
  a.algebra.matrix_products += b.algebra.matrix_products;
  a.algebra.basis_reductions += b.algebra.basis_reductions;
  a.algebra.basis_insertions += b.algebra.basis_insertions;
  a.algebra.cow_detaches += b.algebra.cow_detaches;
  a.algebra.intersection_tests += b.algebra.intersection_tests;
  a.algebra.intersection_fast_paths += b.algebra.intersection_fast_paths;
  a.algebra.intersection_us += b.algebra.intersection_us;
  a.algebra.certificate_us += b.algebra.certificate_us;
}
bool selected(const HistoryComparison &c, const HistoryObserver &o,
              ComparisonMode mode) {
  switch (mode) {
  case ComparisonMode::Joint:
    return c.mayReach();
  case ComparisonMode::Independent:
    return c.independentMayReach(o);
  case ComparisonMode::Projection:
    return c.spdsMayReach();
  }
  throw std::invalid_argument("invalid AffineSPDS comparison mode");
}
Automaton saturate(const System &system, State anchor,
                   spds::Direction direction, spds::Limits limits,
                   std::shared_ptr<AlgebraStatistics> statistics) {
  auto seed = spds::RegularSet::singleton(system.controls(), {anchor, {0}});
  spds::SaturationSession<AffineSemiring> session(
      system, seed, direction, limits,
      system.domain().withStatistics(std::move(statistics)));
  session.run();
  return session.takeResult();
}
} // namespace

HistoryComparison::HistoryComparison(
    AffineSpace calls, AffineSpace fields,
    std::shared_ptr<AlgebraStatistics> statistics)
    : calls_(std::move(calls)), fields_(std::move(fields)),
      statistics_(std::move(statistics)) {
  if (calls_.dimension() != fields_.dimension())
    throw std::invalid_argument("history dimension mismatch");
}
Verdict HistoryComparison::verdict() const {
  if (!verdict_) {
    const auto started = Clock::now();
    const auto certificate_before =
        statistics_ ? statistics_->certificate_us : 0;
    if (!spdsMayReach()) {
      verdict_ = Verdict::ProjectionRejected;
      certificate_.emplace(std::nullopt);
    } else {
      // Keep the small separating equation, not an entire factorization.
      // A later certificate() call performs no second elimination.
      certificate_ = separate(calls_, fields_, statistics_.get());
      verdict_ = *certificate_ ? Verdict::AffineSeparated : Verdict::MayReach;
    }
    if (statistics_)
      statistics_->intersection_us +=
          elapsed(started) - (statistics_->certificate_us - certificate_before);
  }
  return *verdict_;
}
bool HistoryComparison::mayReach() const {
  return verdict() == Verdict::MayReach;
}
const std::optional<SeparationCertificate> &
HistoryComparison::certificate() const {
  (void)verdict();
  return *certificate_;
}
bool HistoryComparison::independentMayReach(
    const HistoryObserver &observer) const {
  if (observer.dimension() != calls_.dimension())
    throw std::invalid_argument("observer/history mismatch");
  if (independent_ && independent_->first == observer.blocks())
    return independent_->second;
  const auto start = Clock::now();
  bool result = spdsMayReach();
  if (result)
    for (const auto &block : observer.blocks())
      if (!calls_.block(block.first, block.second)
               .intersects(fields_.block(block.first, block.second),
                           statistics_.get())) {
        result = false;
        break;
      }
  independent_ = std::make_pair(observer.blocks(), result);
  if (statistics_)
    statistics_->intersection_us += elapsed(start);
  return result;
}
HistoryComparison synchronize(const Automaton &calls,
                              const spds::Configuration &call_query,
                              const Automaton &fields,
                              const spds::Configuration &field_query) {
  if (calls.direction() != fields.direction())
    throw std::invalid_argument("mixed post/pre synchronization");
  return HistoryComparison(
      calls.weight(call_query.control, call_query.stack),
      fields.weight(field_query.control, field_query.stack),
      calls.domain().statistics());
}

QueryResult::QueryResult(
    Vertex anchor, spds::Direction direction, Options options,
    std::shared_ptr<const std::map<Vertex, State>> controls,
    std::shared_ptr<const HistoryObserver> observer, Automaton calls,
    Automaton fields)
    : anchor_(anchor), direction_(direction), options_(options),
      controls_(std::move(controls)), observer_(std::move(observer)),
      calls_(std::move(calls)), fields_(std::move(fields)) {
  statistics_.matrix_dimension = observer_->dimension();
  statistics_.coordinate_dimension = calls_.domain().coordinates();
  for (const auto *automaton : {&calls_, &fields_}) {
    add(statistics_.saturation, automaton->statistics());
    for (const auto &entry : automaton->transitions())
      statistics_.maximum_affine_rank =
          std::max(statistics_.maximum_affine_rank, entry.weight.rank());
  }
}
Statistics QueryResult::statistics() const {
  auto result = statistics_;
  result.saturation.readout_microseconds =
      calls_.statistics().readout_microseconds +
      fields_.statistics().readout_microseconds;
  if (calls_.domain().statistics())
    result.algebra = *calls_.domain().statistics();
  return result;
}
void QueryResult::prepareReadout() const {
  if (call_weights_ && field_weights_)
    return;
  if (!call_weights_)
    call_weights_ = options_.parentheses == spds::StackAcceptance::Empty
                        ? calls_.controlWeights({0})
                        : calls_.controlWeightsWithPrefix({});
  if (!field_weights_)
    field_weights_ = options_.brackets == spds::StackAcceptance::Empty
                         ? fields_.controlWeights({0})
                         : fields_.controlWeightsWithPrefix({});
  for (const auto *weights : {&*call_weights_, &*field_weights_})
    for (const auto &weight : *weights)
      statistics_.maximum_affine_rank =
          std::max(statistics_.maximum_affine_rank, weight.rank());
}
AffineSpace QueryResult::history(const Automaton &automaton, Vertex vertex,
                                 spds::StackAcceptance acceptance) const {
  const auto found = controls_->find(vertex);
  if (found == controls_->end())
    return automaton.domain().zero();
  const auto &weights = &automaton == &calls_ ? call_weights_ : field_weights_;
  if (weights)
    return (*weights)[found->second];
  return acceptance == spds::StackAcceptance::Empty
             ? automaton.weight(found->second, {0})
             : automaton.weightWithPrefix(found->second, {});
}
const HistoryComparison &QueryResult::compare(Vertex vertex) const {
  if (!controls_->count(vertex)) {
    if (!missing_)
      missing_.emplace(calls_.domain().zero(), fields_.domain().zero());
    return *missing_;
  }
  auto found = cache_.find(vertex);
  if (found == cache_.end()) {
    if (options_.readout_batch_threshold &&
        cache_.size() >= options_.readout_batch_threshold)
      prepareReadout();
    found =
        cache_
            .emplace(vertex, HistoryComparison(
                                 history(calls_, vertex, options_.parentheses),
                                 history(fields_, vertex, options_.brackets),
                                 calls_.domain().statistics()))
            .first;
    statistics_.maximum_affine_rank = std::max(
        {statistics_.maximum_affine_rank, found->second.callHistory().rank(),
         found->second.fieldHistory().rank()});
  }
  return found->second;
}
HistoryComparison
QueryResult::compareStacks(Vertex vertex, const std::vector<unsigned> &calls,
                           const std::vector<unsigned> &fields) const {
  const auto found = controls_->find(vertex);
  if (found == controls_->end())
    return HistoryComparison(calls_.domain().zero(), fields_.domain().zero());
  const StackKey key{vertex, calls, fields};
  if (const auto cached = stack_cache_.find(key); cached != stack_cache_.end())
    return cached->second;
  auto result = synchronize(calls_, {found->second, word(calls)}, fields_,
                            {found->second, word(fields)});
  statistics_.maximum_affine_rank =
      std::max({statistics_.maximum_affine_rank, result.callHistory().rank(),
                result.fieldHistory().rank()});
  if (stack_cache_.size() == 64)
    stack_cache_.erase(stack_cache_.begin());
  stack_cache_.emplace(key, result);
  return result;
}

struct PreparedAnalysis::Storage {
  struct DenseEdge {
    State from, to;
  };
  struct WeightedEdge {
    AffineSpace weight;
    AffineSemiring::PreparedWeight prepared;
  };
  struct Projection {
    explicit Projection(const AffineSemiring &domain)
        : calls(domain), fields(domain) {}
    std::shared_ptr<const std::map<Vertex, State>> controls;
    System calls, fields;
  };
  Options options;
  std::shared_ptr<const HistoryObserver> observer;
  AffineSemiring domain;
  std::map<Vertex, State> controls;
  std::vector<Vertex> vertices;
  std::vector<Edge> edges;
  std::vector<DenseEdge> dense_edges;
  std::vector<std::vector<std::size_t>> outgoing, incoming;
  std::vector<State> scc;
  std::optional<spds::PreparedAnalysis> boolean;
  std::uint64_t preparation_us = 0, observer_us = 0;
  // Only slice compilation touches these caches. Independent saturations and
  // readouts have distinct counters and run outside this lock.
  mutable std::mutex mutex;
  mutable std::unordered_map<std::size_t, std::shared_ptr<const WeightedEdge>>
      weights;
  mutable std::shared_ptr<const WeightedEdge> identity_weight;
  mutable std::map<std::pair<spds::Direction, State>,
                   std::shared_ptr<const Projection>>
      slices;
  mutable std::deque<std::pair<spds::Direction, State>> order;
  mutable std::size_t cached_rules = 0;

  Storage(Options opts, const Graph &graph,
          std::shared_ptr<const HistoryObserver> obs)
      : options(opts), observer(std::move(obs)),
        domain(observer->dimension(),
               options.compress_observer ? observer->compactLayout() : nullptr),
        vertices(graph.vertices()), edges(graph.edges()) {
    std::sort(vertices.begin(), vertices.end());
    std::sort(edges.begin(), edges.end(), EdgeLess{});
    outgoing.resize(vertices.size());
    incoming.resize(vertices.size());
    for (State v = 0; v < vertices.size(); ++v)
      controls.emplace(vertices[v], v);
    for (const auto &edge : edges) {
      const auto from = controls.at(edge.source), to = controls.at(edge.target);
      const auto id = dense_edges.size();
      dense_edges.push_back({from, to});
      outgoing[from].push_back(id);
      incoming[to].push_back(id);
    }
    // Iterative Kosaraju: SCC members share each directional structural slice.
    std::vector<bool> seen(vertices.size(), false);
    std::vector<State> finish;
    for (State root = 0; root < vertices.size(); ++root) {
      if (seen[root])
        continue;
      seen[root] = true;
      std::vector<std::pair<State, std::size_t>> stack{{root, 0}};
      while (!stack.empty()) {
        auto &frame = stack.back();
        if (frame.second == outgoing[frame.first].size()) {
          finish.push_back(frame.first);
          stack.pop_back();
          continue;
        }
        const State next =
            dense_edges[outgoing[frame.first][frame.second++]].to;
        if (!seen[next]) {
          seen[next] = true;
          stack.emplace_back(next, 0);
        }
      }
    }
    scc.assign(vertices.size(), Missing);
    for (auto root = finish.rbegin(); root != finish.rend(); ++root) {
      if (scc[*root] != Missing)
        continue;
      std::vector<State> work{*root};
      scc[*root] = *root;
      for (std::size_t cursor = 0; cursor < work.size(); ++cursor)
        for (auto id : incoming[work[cursor]]) {
          const auto next = dense_edges[id].from;
          if (scc[next] == Missing) {
            scc[next] = *root;
            work.push_back(next);
          }
        }
    }
    bool identity = true;
    for (const auto &entry : observer->assignments())
      if (!entry.second.isIdentity()) {
        identity = false;
        break;
      }
    if (identity) {
      spds::Options baseline;
      baseline.parentheses = options.parentheses;
      baseline.brackets = options.brackets;
      baseline.limits = options.limits;
      boolean.emplace(spds::Solver(baseline).prepare(graph));
    }
  }
  std::vector<bool> reachable(const std::vector<State> &seeds,
                              spds::Direction direction) const {
    const auto &adjacency =
        direction == spds::Direction::Post ? outgoing : incoming;
    std::vector<bool> live(vertices.size(), false);
    std::vector<State> work;
    for (State seed : seeds)
      if (!live[seed]) {
        live[seed] = true;
        work.push_back(seed);
      }
    for (std::size_t cursor = 0; cursor < work.size(); ++cursor)
      for (auto id : adjacency[work[cursor]]) {
        const auto next = direction == spds::Direction::Post
                              ? dense_edges[id].to
                              : dense_edges[id].from;
        if (!live[next]) {
          live[next] = true;
          work.push_back(next);
        }
      }
    return live;
  }
  std::shared_ptr<const WeightedEdge> weight(std::size_t id,
                                             Statistics &statistics) const {
    if (const auto found = weights.find(id); found != weights.end())
      return found->second;
    const auto &matrix = observer->matrix(edges[id]);
    if (matrix.isIdentity()) {
      if (!identity_weight) {
        ++statistics.prepared_weights;
        identity_weight = std::make_shared<const WeightedEdge>(
            WeightedEdge{domain.one(), nullptr});
      }
      return identity_weight;
    }
    ++statistics.prepared_weights;
    auto value = domain.lift(matrix);
    auto prepared = domain.prepareWeight(value);
    auto entry = std::make_shared<const WeightedEdge>(
        WeightedEdge{std::move(value), std::move(prepared)});
    weights.emplace(id, entry);
    return entry;
  }
  static System::Rule rule(const Edge &edge, const WeightedEdge &value,
                           bool call) {
    const auto open =
        call ? LabelKind::OpenParenthesis : LabelKind::OpenBracket;
    const auto close =
        call ? LabelKind::CloseParenthesis : LabelKind::CloseBracket;
    if (edge.label.kind == close)
      return {System::RuleKind::Exact,
              0,
              symbol(edge.label.id),
              0,
              {},
              value.weight,
              value.prepared};
    if (edge.label.kind == open)
      return {System::RuleKind::PushAny,
              0,
              0,
              0,
              {symbol(edge.label.id)},
              value.weight,
              value.prepared};
    return {System::RuleKind::PreserveAny,
            0,
            0,
            0,
            {},
            value.weight,
            value.prepared};
  }
  std::shared_ptr<const Projection>
  project(Vertex anchor, spds::Direction direction,
          const std::vector<Vertex> *endpoints, Statistics &statistics) const {
    std::lock_guard<std::mutex> lock(mutex);
    const State start = controls.at(anchor);
    const auto key = std::make_pair(direction, scc[start]);
    if (!endpoints)
      if (const auto found = slices.find(key); found != slices.end()) {
        ++statistics.slice_cache_hits;
        return found->second;
      }
    auto live = reachable({start}, direction);
    if (endpoints) {
      std::vector<State> destinations;
      for (Vertex v : *endpoints)
        destinations.push_back(controls.at(v));
      const auto reverse =
          reachable(destinations, direction == spds::Direction::Post
                                      ? spds::Direction::Pre
                                      : spds::Direction::Post);
      for (State v = 0; v < vertices.size(); ++v)
        live[v] = live[v] && reverse[v];
      live[start] = true;
    }
    auto projection = std::make_shared<Projection>(domain);
    auto local = std::make_shared<std::map<Vertex, State>>();
    std::vector<State> indices(vertices.size(), Missing);
    for (State v = 0; v < vertices.size(); ++v)
      if (live[v]) {
        const auto id = projection->calls.addControl();
        projection->fields.addControl();
        indices[v] = id;
        local->emplace(vertices[v], id);
      }
    projection->controls = local;
    for (State v = 0; v < vertices.size(); ++v)
      if (live[v])
        for (auto id : outgoing[v]) {
          const auto &edge = dense_edges[id];
          if (!live[edge.to])
            continue;
          const auto value = weight(id, statistics);
          projection->calls.addMappedRule(rule(edges[id], *value, true),
                                          indices[edge.from], indices[edge.to]);
          projection->fields.addMappedRule(rule(edges[id], *value, false),
                                           indices[edge.from],
                                           indices[edge.to]);
        }
    const auto count =
        projection->calls.rules().size() + projection->fields.rules().size();
    statistics.compiled_rules += count;
    if (!endpoints && options.max_cached_slices &&
        options.max_cached_slice_rules &&
        count <= options.max_cached_slice_rules) {
      while (!order.empty() &&
             (slices.size() >= options.max_cached_slices ||
              count > options.max_cached_slice_rules - cached_rules)) {
        const auto oldest = order.front();
        order.pop_front();
        const auto &p = slices.at(oldest);
        cached_rules -= p->calls.rules().size() + p->fields.rules().size();
        slices.erase(oldest);
      }
      slices.emplace(key, projection);
      order.push_back(key);
      cached_rules += count;
    }
    return projection;
  }
};

PreparedAnalysis::PreparedAnalysis(std::shared_ptr<Storage> storage)
    : storage_(std::move(storage)) {}
const HistoryObserver &PreparedAnalysis::observer() const {
  return *storage_->observer;
}
QueryResult
PreparedAnalysis::query(Vertex anchor, spds::Direction direction,
                        const std::vector<Vertex> *endpoints) const {
  if (!storage_->controls.count(anchor))
    throw std::invalid_argument("AffineSPDS query vertex is not in graph");
  const auto start = Clock::now();
  Statistics compilation;
  auto projection =
      storage_->project(anchor, direction, endpoints, compilation);
  const auto projection_us = elapsed(start);
  auto statistics = std::make_shared<AlgebraStatistics>();
  const auto state = projection->controls->at(anchor);
  auto calls = saturate(projection->calls, state, direction,
                        storage_->options.limits, statistics);
  auto fields = saturate(projection->fields, state, direction,
                         storage_->options.limits, statistics);
  QueryResult result(anchor, direction, storage_->options, projection->controls,
                     storage_->observer, std::move(calls), std::move(fields));
  result.statistics_.saturation.projection_microseconds = projection_us;
  result.statistics_.slice_cache_hits = compilation.slice_cache_hits;
  result.statistics_.compiled_rules = compilation.compiled_rules;
  result.statistics_.prepared_weights = compilation.prepared_weights;
  result.statistics_.observer_microseconds = storage_->observer_us;
  return result;
}
QueryResult PreparedAnalysis::queryFrom(Vertex source) const {
  return query(source, spds::Direction::Post);
}
QueryResult PreparedAnalysis::queryTo(Vertex target) const {
  return query(target, spds::Direction::Pre);
}

Result PreparedAnalysis::analyzeFrom(Vertex source, ComparisonMode mode) const {
  if (storage_->boolean) {
    auto baseline = storage_->boolean->analyzeFrom(source);
    Result result{std::move(baseline.upper_bound), mode, {}};
    result.statistics.saturation = baseline.statistics;
    result.statistics.matrix_dimension = observer().dimension();
    result.statistics.coordinate_dimension = storage_->domain.coordinates();
    result.statistics.observer_microseconds = storage_->observer_us;
    result.statistics.saturation.projection_microseconds =
        storage_->preparation_us;
    return result;
  }
  auto query = queryFrom(source);
  query.prepareReadout();
  Result result;
  result.mode = mode;
  for (const auto &entry : *query.controls_)
    if (selected(query.compare(entry.first), observer(), mode))
      result.pairs.insert({source, entry.first});
  result.statistics = query.statistics();
  result.statistics.saturation.projection_microseconds +=
      storage_->preparation_us;
  return result;
}
Result PreparedAnalysis::analyzeTo(Vertex target, ComparisonMode mode) const {
  if (storage_->boolean) {
    auto baseline = storage_->boolean->analyzeTo(target);
    Result result{std::move(baseline.upper_bound), mode, {}};
    result.statistics.saturation = baseline.statistics;
    result.statistics.matrix_dimension = observer().dimension();
    result.statistics.coordinate_dimension = storage_->domain.coordinates();
    result.statistics.observer_microseconds = storage_->observer_us;
    result.statistics.saturation.projection_microseconds =
        storage_->preparation_us;
    return result;
  }
  auto query = queryTo(target);
  query.prepareReadout();
  Result result;
  result.mode = mode;
  for (const auto &entry : *query.controls_)
    if (selected(query.compare(entry.first), observer(), mode))
      result.pairs.insert({entry.first, target});
  result.statistics = query.statistics();
  result.statistics.saturation.projection_microseconds +=
      storage_->preparation_us;
  return result;
}
Result PreparedAnalysis::analyzeAll(ComparisonMode mode) const {
  Result result;
  result.mode = mode;
  result.statistics.matrix_dimension = observer().dimension();
  result.statistics.coordinate_dimension = storage_->domain.coordinates();
  result.statistics.observer_microseconds = storage_->observer_us;
  result.statistics.saturation.projection_microseconds =
      storage_->preparation_us;
  if (storage_->boolean) {
    auto baseline = storage_->boolean->analyzeAll();
    result.pairs = std::move(baseline.upper_bound);
    result.statistics.saturation = baseline.statistics;
    result.statistics.saturation.projection_microseconds =
        storage_->preparation_us;
    return result;
  }
  for (Vertex source : storage_->vertices) {
    auto query = queryFrom(source); // Never saturate unrelated weak components.
    query.prepareReadout();
    for (const auto &target : *query.controls_)
      if (selected(query.compare(target.first), observer(), mode))
        result.pairs.insert({source, target.first});
    add(result.statistics, query.statistics());
  }
  return result;
}
Result PreparedAnalysis::analyzeDemands(const std::vector<Pair> &demands,
                                        ComparisonMode mode,
                                        spds::DemandDirection direction) const {
  std::map<Vertex, std::vector<Vertex>> sources, targets;
  PairSet unique;
  for (const auto &pair : demands) {
    if (!storage_->controls.count(pair.source) ||
        !storage_->controls.count(pair.target))
      throw std::invalid_argument("AffineSPDS demand vertex is not in graph");
    if (unique.insert(pair).second) {
      sources[pair.source].push_back(pair.target);
      targets[pair.target].push_back(pair.source);
    }
  }
  Result result;
  result.mode = mode;
  result.statistics.matrix_dimension = observer().dimension();
  result.statistics.coordinate_dimension = storage_->domain.coordinates();
  result.statistics.observer_microseconds = storage_->observer_us;
  result.statistics.saturation.projection_microseconds =
      storage_->preparation_us;
  if (storage_->boolean) {
    auto baseline = storage_->boolean->analyzeDemands(demands, direction);
    result.pairs = std::move(baseline.upper_bound);
    result.statistics.saturation = baseline.statistics;
    result.statistics.saturation.projection_microseconds =
        storage_->preparation_us;
    return result;
  }
  const bool post = direction == spds::DemandDirection::Post ||
                    (direction == spds::DemandDirection::Auto &&
                     sources.size() <= targets.size());
  for (const auto &group : post ? sources : targets) {
    auto query = this->query(
        group.first, post ? spds::Direction::Post : spds::Direction::Pre,
        &group.second);
    if (storage_->options.readout_batch_threshold &&
        group.second.size() >= storage_->options.readout_batch_threshold)
      query.prepareReadout();
    for (Vertex endpoint : group.second)
      if (selected(query.compare(endpoint), observer(), mode))
        result.pairs.insert(post ? Pair{group.first, endpoint}
                                 : Pair{endpoint, group.first});
    add(result.statistics, query.statistics());
  }
  return result;
}

void Solver::validate(const Graph &graph,
                      const HistoryObserver &observer) const {
  if (options_.max_matrix_dimension &&
      observer.dimension() > options_.max_matrix_dimension)
    throw spds::ResourceLimit("AffineSPDS matrix-dimension limit exceeded");
  observer.validate(graph);
  for (const auto &edge : graph.edges())
    switch (edge.label.kind) {
    case LabelKind::OpenParenthesis:
    case LabelKind::CloseParenthesis:
    case LabelKind::OpenBracket:
    case LabelKind::CloseBracket:
    case LabelKind::Neutral:
      break;
    default:
      throw std::invalid_argument("invalid AffineSPDS graph label kind");
    }
}
PreparedAnalysis Solver::prepare(const Graph &graph) const {
  auto opts = options_.observer;
  if (options_.max_matrix_dimension &&
      (!opts.max_matrix_dimension ||
       opts.max_matrix_dimension > options_.max_matrix_dimension))
    opts.max_matrix_dimension = options_.max_matrix_dimension;
  const auto start = Clock::now();
  HistoryObserver observer;
  try {
    observer = HistoryObserver::automatic(graph, opts);
  } catch (const std::length_error &e) {
    throw spds::ResourceLimit(e.what());
  }
  const auto observer_us = elapsed(start);
  auto result = prepare(graph, observer);
  result.storage_->observer_us = observer_us;
  return result;
}
PreparedAnalysis Solver::prepare(const Graph &graph,
                                 const HistoryObserver &observer) const {
  const auto start = Clock::now();
  validate(graph, observer);
  auto storage = std::make_shared<PreparedAnalysis::Storage>(
      options_, graph, std::make_shared<const HistoryObserver>(observer));
  storage->preparation_us = elapsed(start);
  return PreparedAnalysis(std::move(storage));
}
} // namespace lotus::cfl::interleaved_dyck::affine

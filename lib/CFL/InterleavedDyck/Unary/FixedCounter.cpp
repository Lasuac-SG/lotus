#include "CFL/InterleavedDyck/Unary/FixedCounter.h"

#include "CFL/InterleavedDyck/Unary/Support.h"

#include <numeric>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::unary {
namespace {

using namespace detail;

std::size_t counterBound(std::size_t n) {
  const auto square = checkedMultiply(n, n, "fixed-counter bound");
  return checkedAdd(checkedMultiply(18, square, "fixed-counter bound"),
                    checkedMultiply(6, n, "fixed-counter bound"),
                    "fixed-counter bound");
}

struct ComputedResult {
  std::unordered_map<Vertex, std::size_t> components;
  FixedCounterStats stats;
};

ComputedResult makeResult(const UnaryProjection &projection,
                          const UnaryGraph &processed,
                          const std::vector<std::size_t> &original_to_processed,
                          bool sparsified) {
  ComputedResult result;
  auto &stats = result.stats;
  stats.input_vertices = projection.graph.vertex_count;
  stats.input_arcs = projection.original_arc_count;
  stats.quotient_vertices = processed.vertex_count;
  stats.quotient_arcs = processed.edges.size();
  stats.added_reverse_arcs = projection.added_reverse_arcs;
  stats.input_was_bidirected = projection.added_reverse_arcs == 0;
  stats.overapproximates_original = projection.added_reverse_arcs != 0;
  stats.sparsified = sparsified;

  auto begin = Clock::now();
  const auto parts = splitWeakComponents(processed);
  stats.execution.decomposition_us = elapsed(begin);
  stats.execution.weak_components = parts.size();
  std::vector<std::size_t> processed_components(processed.vertex_count);
  std::size_t offset = 0;
  begin = Clock::now();
  for (const auto &part : parts) {
    const auto n = part.graph.vertex_count;
    stats.execution.largest_component_vertices =
        std::max(stats.execution.largest_component_vertices, n);
    std::vector<std::size_t> local;
    if (n == 1) {
      local = {0};
      ++stats.execution.trivial_components;
    } else if (part.counter_mask != 3) {
      auto single = singleCounter(part.graph);
      accumulate(stats.single_counter_dyck, single.stats);
      stats.execution.peak_working_bytes = std::max(
          stats.execution.peak_working_bytes, single.stats.peak_working_bytes);
      local = std::move(single.component);
      ++stats.execution.single_counter_components;
    } else {
      const auto bound = counterBound(n);
      const LiftedCounterGraph view(part.graph, bound, false);
      const auto dyck = view.solve();
      stats.counter_bound = std::max(stats.counter_bound, bound);
      stats.control_states += view.states;
      stats.translated_arcs += view.arcs;
      stats.epsilon_edges += view.epsilon_edges;
      stats.closing_edges += view.closing_edges;
      accumulate(stats.dyck, dyck.stats);
      stats.execution.peak_working_bytes = std::max(
          stats.execution.peak_working_bytes, dyck.stats.peak_working_bytes);
      local = zeroComponents(dyck, n, view.width);
      // Only original zero states need output IDs, never a state-space-sized
      // identifiers array. Include their mapping storage in the estimate.
      stats.execution.peak_working_bytes =
          std::max(stats.execution.peak_working_bytes,
                   dyck.component.capacity() * sizeof(std::size_t) +
                       n * (sizeof(std::size_t) * 3 + sizeof(void *) * 2));
    }
    std::size_t count = 0;
    for (std::size_t v = 0; v < n; ++v) {
      processed_components[part.original_vertices[v]] = offset + local[v];
      count = std::max(count, local[v] + 1);
    }
    offset += count;
  }
  stats.execution.solving_us = elapsed(begin);
  begin = Clock::now();
  std::vector<std::size_t> identifiers(offset, kNone);
  std::size_t count = 0;
  result.components.reserve(projection.vertices.size());
  for (std::size_t v = 0; v < projection.vertices.size(); ++v) {
    const auto component = processed_components[original_to_processed[v]];
    if (identifiers[component] == kNone)
      identifiers[component] = count++;
    result.components.emplace(projection.vertices[v], identifiers[component]);
  }
  stats.execution.lifting_us = elapsed(begin);
  return result;
}

} // namespace

std::size_t FixedCounterResult::component(Vertex vertex) const {
  const auto found = components_.find(vertex);
  if (found == components_.end())
    throw std::out_of_range("unknown unary interleaved-Dyck vertex");
  return found->second;
}

bool FixedCounterResult::connected(Vertex first, Vertex second) const {
  return component(first) == component(second);
}

FixedCounterResult
FixedCounterSolver::solve(const Graph &graph,
                          const FixedCounterOptions &options) const {
  const auto start = Clock::now();
  const auto projection = projectToUnary(graph, options.input_policy);
  const auto projection_us = elapsed(start);
  ComputedResult computed;
  std::uint64_t preprocessing_us = 0;
  if (options.sparsify) {
    const auto begin = Clock::now();
    const auto quotient = sparsifyUnaryGraph(projection.graph);
    preprocessing_us = elapsed(begin);
    computed = makeResult(projection, quotient.graph,
                          quotient.original_to_quotient, true);
    computed.stats.quotient_dyck = quotient.dyck;
    computed.stats.execution.peak_working_bytes =
        std::max(computed.stats.execution.peak_working_bytes,
                 quotient.dyck.peak_working_bytes);
  } else {
    std::vector<std::size_t> identity(projection.graph.vertex_count);
    std::iota(identity.begin(), identity.end(), std::size_t(0));
    computed = makeResult(projection, projection.graph, identity, false);
  }
  computed.stats.execution.projection_us = projection_us;
  computed.stats.execution.preprocessing_us = preprocessing_us;
  computed.stats.execution.total_us = elapsed(start);
  FixedCounterResult result;
  result.components_ = std::move(computed.components);
  result.stats_ = computed.stats;
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::unary

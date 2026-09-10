#pragma once

#include "CFL/InterleavedDyck/Core/UnaryGraph.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace lotus::cfl::interleaved_dyck::unary::detail {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

inline std::uint64_t elapsed(Clock::time_point begin) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                               begin)
      .count();
}

inline std::size_t checkedAdd(std::size_t a, std::size_t b,
                              const char *description) {
  if (b > kNone - a)
    throw std::overflow_error(std::string(description) + " is too large");
  return a + b;
}

inline std::size_t checkedMultiply(std::size_t a, std::size_t b,
                                   const char *description) {
  if (a && b > kNone / a)
    throw std::overflow_error(std::string(description) + " is too large");
  return a * b;
}

inline void accumulate(BidirectedDyckStats &total,
                       const BidirectedDyckStats &part) {
  total.states += part.states;
  total.epsilon_edges += part.epsilon_edges;
  total.closing_edges += part.closing_edges;
  total.worklist_pops += part.worklist_pops;
  total.component_unions += part.component_unions;
  total.components += part.components;
  total.epsilon_components += part.epsilon_components;
  total.stored_closing_edges += part.stored_closing_edges;
  total.scanned_closing_edges += part.scanned_closing_edges;
  total.peak_working_bytes =
      std::max(total.peak_working_bytes, part.peak_working_bytes);
  total.epsilon_us += part.epsilon_us;
  total.closing_us += part.closing_us;
  total.saturation_us += part.saturation_us;
}

// A lifted one-counter graph is an edge generator over the original graph.
// The bounded counter is stored in finite control; the other stays unbounded.
// No expanded epsilon or closing edge vector is constructed.
class LiftedCounterGraph {
public:
  LiftedCounterGraph(const UnaryGraph &graph, std::size_t bound,
                     bool bound_first)
      : graph_(graph), width(checkedAdd(bound, 1, "counter width")),
        states(
            checkedMultiply(graph.vertex_count, width, "counter state space")),
        bounded_(bound_first ? UnaryLabel::CloseFirst
                             : UnaryLabel::CloseSecond),
        free_(bound_first ? UnaryLabel::CloseSecond : UnaryLabel::CloseFirst) {
    for (const auto &edge : graph.edges) {
      const bool bounded =
          edge.label == bounded_ || edge.label == complement(bounded_);
      arcs = checkedAdd(arcs, bounded ? bound : width, "translated arcs");
      if (edge.label == free_)
        closing_edges = checkedAdd(closing_edges, width, "closing edges");
      if (edge.label == bounded_)
        epsilon_edges = checkedAdd(epsilon_edges, bound, "epsilon edges");
      if (edge.label == UnaryLabel::Epsilon && edge.source < edge.target)
        epsilon_edges = checkedAdd(epsilon_edges, width, "epsilon edges");
    }
  }

  void
  epsilon(const BidirectedDyckComponentSolver::EpsilonVisitor &visit) const {
    for (const auto &edge : graph_.edges) {
      if (edge.label == UnaryLabel::Epsilon && edge.source < edge.target) {
        for (std::size_t h = 0; h < width; ++h)
          visit(edge.source * width + h, edge.target * width + h);
      } else if (edge.label == bounded_) {
        for (std::size_t h = 1; h < width; ++h)
          visit(edge.source * width + h, edge.target * width + h - 1);
      }
    }
  }

  void
  closing(const BidirectedDyckComponentSolver::ClosingVisitor &visit) const {
    for (const auto &edge : graph_.edges)
      if (edge.label == free_)
        for (std::size_t h = 0; h < width; ++h)
          visit(edge.source * width + h, edge.target * width + h, 0);
  }

  BidirectedDyckResult solve(bool retain_quotient = false) const {
    return BidirectedDyckComponentSolver{}.solveGenerated(
        states, 1, [&](const auto &visit) { epsilon(visit); },
        [&](const auto &visit) { closing(visit); }, closing_edges,
        retain_quotient);
  }

private:
  const UnaryGraph &graph_;

public:
  const std::size_t width;
  const std::size_t states;
  std::size_t arcs = 0;
  std::size_t epsilon_edges = 0;
  std::size_t closing_edges = 0;

private:
  UnaryLabel bounded_;
  UnaryLabel free_;
};

inline BidirectedDyckResult singleCounter(const UnaryGraph &graph) {
  return BidirectedDyckComponentSolver{}.solveGenerated(
      graph.vertex_count, 1,
      [&](const auto &visit) {
        for (const auto &edge : graph.edges)
          if (edge.label == UnaryLabel::Epsilon && edge.source < edge.target)
            visit(edge.source, edge.target);
      },
      [&](const auto &visit) {
        for (const auto &edge : graph.edges)
          if (edge.label == UnaryLabel::CloseFirst ||
              edge.label == UnaryLabel::CloseSecond)
            visit(edge.source, edge.target, 0);
      },
      graph.edges.size() / 2);
}

inline std::vector<std::size_t> zeroComponents(const BidirectedDyckResult &dyck,
                                               std::size_t vertices,
                                               std::size_t width) {
  std::unordered_map<std::size_t, std::size_t> identifiers;
  identifiers.reserve(vertices);
  std::vector<std::size_t> result(vertices);
  for (std::size_t v = 0; v < vertices; ++v)
    result[v] =
        identifiers.emplace(dyck.component[v * width], identifiers.size())
            .first->second;
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::unary::detail

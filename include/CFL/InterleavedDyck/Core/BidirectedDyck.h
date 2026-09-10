#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace lotus::cfl::interleaved_dyck {

struct StatePair {
  std::size_t source = 0;
  std::size_t target = 0;
};

struct LabeledStateEdge {
  std::size_t source = 0;
  std::size_t target = 0;
  std::size_t label = 0;
};

struct BidirectedDyckStats {
  std::size_t states = 0;
  std::size_t epsilon_edges = 0;
  std::size_t closing_edges = 0;
  std::size_t worklist_pops = 0;
  std::size_t component_unions = 0;
  std::size_t components = 0;
  std::size_t epsilon_components = 0;
  std::size_t stored_closing_edges = 0;
  std::size_t scanned_closing_edges = 0;
  // Peak container payload, excluding caller-owned input and allocator
  // overhead.
  std::size_t peak_working_bytes = 0;
  std::uint64_t epsilon_us = 0;
  std::uint64_t closing_us = 0;
  std::uint64_t saturation_us = 0;
};

struct BidirectedDyckResult {
  // Dense IDs in [0, stats.components), not original state representatives.
  std::vector<std::size_t> component;
  BidirectedDyckStats stats;
  // Optional final functional closing transitions, in dense component IDs.
  std::vector<LabeledStateEdge> quotient_closing_edges;
};

/// Compute zero-height Dyck components of a bidirected one-counter graph.
///
/// Only closing edges are represented explicitly; bidirectedness supplies the
/// corresponding opening reverses. `label_count` supports the fixed-alphabet
/// multi-type closure used during quotient sparsification.
class BidirectedDyckComponentSolver {
public:
  using EpsilonVisitor = std::function<void(std::size_t, std::size_t)>;
  using ClosingVisitor =
      std::function<void(std::size_t, std::size_t, std::size_t)>;
  using EpsilonSource = std::function<void(const EpsilonVisitor &)>;
  using ClosingSource = std::function<void(const ClosingVisitor &)>;

  // Sources are invoked synchronously once each. Emit each undirected epsilon
  // connection once; reverse/opening edges need not be emitted. Epsilon
  // contraction precedes allocation of the closing-edge component tables.
  BidirectedDyckResult solveGenerated(std::size_t state_count,
                                      std::size_t label_count,
                                      const EpsilonSource &epsilon_edges,
                                      const ClosingSource &closing_edges,
                                      std::size_t closing_edge_hint = 0,
                                      bool retain_quotient = false) const;

  BidirectedDyckResult
  solve(std::size_t state_count, std::size_t label_count,
        const std::vector<StatePair> &epsilon_edges,
        const std::vector<LabeledStateEdge> &closing_edges) const;
};

} // namespace lotus::cfl::interleaved_dyck

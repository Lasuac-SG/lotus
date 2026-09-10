#pragma once

#include "CFL/InterleavedDyck/Core/BidirectedDyck.h"
#include "CFL/InterleavedDyck/Core/Graph.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lotus::cfl::interleaved_dyck {

enum class BidirectedInputPolicy {
  RequireBidirected,
  AddMissingReverseEdges,
};

enum class UnaryLabel : unsigned char {
  Epsilon,
  OpenFirst,
  CloseFirst,
  OpenSecond,
  CloseSecond,
};

UnaryLabel complement(UnaryLabel label);

struct UnaryEdge {
  std::size_t source = 0;
  std::size_t target = 0;
  UnaryLabel label = UnaryLabel::Epsilon;

  bool operator==(const UnaryEdge &other) const;
};

struct UnaryGraph {
  std::size_t vertex_count = 0;
  std::vector<UnaryEdge> edges;
};

struct UnaryWeakComponent {
  UnaryGraph graph;
  std::vector<std::size_t> original_vertices;
  unsigned counter_mask = 0; // Bit 0: first counter; bit 1: second counter.
};

// Each local graph uses dense vertex IDs; components are independent and can
// be solved sequentially. Bidirected graphs have no arcs between components.
std::vector<UnaryWeakComponent> splitWeakComponents(const UnaryGraph &graph);

struct UnaryExecutionStats {
  std::size_t weak_components = 0;
  std::size_t largest_component_vertices = 0;
  std::size_t trivial_components = 0;
  std::size_t single_counter_components = 0;
  // Peak construction container payload, not process RSS. Excludes the input,
  // projected/component graphs, final output map, and allocator overhead.
  std::size_t peak_working_bytes = 0;
  std::uint64_t projection_us = 0;
  std::uint64_t preprocessing_us = 0;
  std::uint64_t decomposition_us = 0;
  std::uint64_t solving_us = 0;
  std::uint64_t lifting_us = 0;
  std::uint64_t total_us = 0;
};

struct UnaryProjection {
  UnaryGraph graph;
  std::vector<Vertex> vertices;
  std::size_t original_arc_count = 0;
  std::size_t added_reverse_arcs = 0;
};

UnaryProjection projectToUnary(const Graph &graph,
                               BidirectedInputPolicy input_policy =
                                   BidirectedInputPolicy::RequireBidirected);

struct UnaryQuotient {
  UnaryGraph graph;
  std::vector<std::size_t> original_to_quotient;
  BidirectedDyckStats dyck;
};

/// Contract epsilon and forced ordinary-Dyck components of a fixed-alphabet
/// bidirected unary graph.
UnaryQuotient sparsifyUnaryGraph(const UnaryGraph &graph);

} // namespace lotus::cfl::interleaved_dyck

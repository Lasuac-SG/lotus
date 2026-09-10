#include "CFL/InterleavedDyck/Core/UnaryGraph.h"

#include "CFL/InterleavedDyck/Core/BidirectedDyck.h"
#include "CFL/InterleavedDyck/Core/DisjointSets.h"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::interleaved_dyck {
namespace {

template <typename T> void hashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

UnaryLabel projectLabel(const Label &label) {
  switch (label.kind) {
  case LabelKind::Neutral:
    return UnaryLabel::Epsilon;
  case LabelKind::OpenParenthesis:
    return UnaryLabel::OpenFirst;
  case LabelKind::CloseParenthesis:
    return UnaryLabel::CloseFirst;
  case LabelKind::OpenBracket:
    return UnaryLabel::OpenSecond;
  case LabelKind::CloseBracket:
    return UnaryLabel::CloseSecond;
  }
  throw std::logic_error("invalid interleaved-Dyck label kind");
}

struct UnaryEdgeHash {
  std::size_t operator()(const UnaryEdge &edge) const {
    std::size_t seed = std::hash<std::size_t>{}(edge.source);
    hashCombine(seed, edge.target);
    hashCombine(seed, static_cast<unsigned char>(edge.label));
    return seed;
  }
};

} // namespace

UnaryLabel complement(UnaryLabel label) {
  switch (label) {
  case UnaryLabel::Epsilon:
    return UnaryLabel::Epsilon;
  case UnaryLabel::OpenFirst:
    return UnaryLabel::CloseFirst;
  case UnaryLabel::CloseFirst:
    return UnaryLabel::OpenFirst;
  case UnaryLabel::OpenSecond:
    return UnaryLabel::CloseSecond;
  case UnaryLabel::CloseSecond:
    return UnaryLabel::OpenSecond;
  }
  throw std::logic_error("invalid unary interleaved-Dyck label");
}

bool UnaryEdge::operator==(const UnaryEdge &other) const {
  return source == other.source && target == other.target &&
         label == other.label;
}

UnaryProjection projectToUnary(const Graph &input,
                               BidirectedInputPolicy input_policy) {
  UnaryProjection result;
  result.vertices = input.vertices();
  result.graph.vertex_count = result.vertices.size();

  std::unordered_map<Vertex, std::size_t> indices;
  indices.reserve(result.vertices.size());
  for (std::size_t i = 0; i < result.vertices.size(); ++i) {
    indices.emplace(result.vertices[i], i);
  }

  std::unordered_set<UnaryEdge, UnaryEdgeHash> unique;
  unique.reserve(input.edges().size());
  for (const Edge &edge : input.edges()) {
    unique.insert({indices.at(edge.source), indices.at(edge.target),
                   projectLabel(edge.label)});
  }
  result.original_arc_count = unique.size();

  result.graph.edges.assign(unique.begin(), unique.end());
  for (std::size_t i = 0; i < result.original_arc_count; ++i) {
    const UnaryEdge edge = result.graph.edges[i];
    const UnaryEdge reverse{edge.target, edge.source, complement(edge.label)};
    if (unique.count(reverse) != 0U) {
      continue;
    }
    if (input_policy == BidirectedInputPolicy::RequireBidirected) {
      throw std::invalid_argument(
          "unary interleaved-Dyck reachability requires a bidirected graph; "
          "missing reverse arc for " +
          std::to_string(result.vertices[edge.source]) + " -> " +
          std::to_string(result.vertices[edge.target]));
    }
    unique.insert(reverse);
    ++result.added_reverse_arcs;
    result.graph.edges.push_back(reverse);
  }
  return result;
}

UnaryQuotient sparsifyUnaryGraph(const UnaryGraph &graph) {
  auto dyck = BidirectedDyckComponentSolver{}.solveGenerated(
      graph.vertex_count, 2,
      [&](const BidirectedDyckComponentSolver::EpsilonVisitor &visit) {
        for (const auto &edge : graph.edges)
          if (edge.label == UnaryLabel::Epsilon && edge.source < edge.target)
            visit(edge.source, edge.target);
      },
      [&](const BidirectedDyckComponentSolver::ClosingVisitor &visit) {
        for (const auto &edge : graph.edges) {
          if (edge.label == UnaryLabel::CloseFirst)
            visit(edge.source, edge.target, 0);
          else if (edge.label == UnaryLabel::CloseSecond)
            visit(edge.source, edge.target, 1);
        }
      },
      graph.edges.size() / 2, true);

  UnaryQuotient result;
  result.dyck = dyck.stats;
  result.graph.vertex_count = dyck.stats.components;
  result.original_to_quotient = std::move(dyck.component);
  result.graph.edges.reserve(2 * dyck.quotient_closing_edges.size());
  for (const auto &edge : dyck.quotient_closing_edges) {
    const auto close =
        edge.label == 0 ? UnaryLabel::CloseFirst : UnaryLabel::CloseSecond;
    result.graph.edges.push_back({edge.source, edge.target, close});
    result.graph.edges.push_back({edge.target, edge.source, complement(close)});
  }
  return result;
}

std::vector<UnaryWeakComponent> splitWeakComponents(const UnaryGraph &graph) {
  std::size_t count = 0;
  std::vector<std::size_t> groups;
  {
    detail::DisjointSets sets(graph.vertex_count);
    for (const auto &edge : graph.edges)
      sets.join(edge.source, edge.target);
    groups = sets.takeComponents(count);
  }
  std::vector<UnaryWeakComponent> result(count);
  std::vector<std::size_t> vertex_counts(count, 0), edge_counts(count, 0);
  for (auto group : groups)
    ++vertex_counts[group];
  for (const auto &edge : graph.edges)
    ++edge_counts[groups[edge.source]];
  for (std::size_t c = 0; c < count; ++c) {
    result[c].original_vertices.reserve(vertex_counts[c]);
    result[c].graph.edges.reserve(edge_counts[c]);
    result[c].graph.vertex_count = vertex_counts[c];
  }
  std::vector<std::size_t> local(graph.vertex_count);
  for (std::size_t v = 0; v < graph.vertex_count; ++v) {
    auto &component = result[groups[v]];
    local[v] = component.original_vertices.size();
    component.original_vertices.push_back(v);
  }
  for (const auto &edge : graph.edges) {
    auto &component = result[groups[edge.source]];
    component.graph.edges.push_back(
        {local[edge.source], local[edge.target], edge.label});
    if (edge.label == UnaryLabel::OpenFirst ||
        edge.label == UnaryLabel::CloseFirst)
      component.counter_mask |= 1;
    if (edge.label == UnaryLabel::OpenSecond ||
        edge.label == UnaryLabel::CloseSecond)
      component.counter_mask |= 2;
  }
  return result;
}

} // namespace lotus::cfl::interleaved_dyck

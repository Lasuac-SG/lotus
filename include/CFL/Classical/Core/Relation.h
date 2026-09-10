#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <llvm/ADT/STLFunctionalExtras.h>

namespace lotus::cfl::classical {

using NodeId = std::size_t;
using SymbolId = std::uint32_t;

struct RelationEdge {
  SymbolId symbol = 0;
  NodeId source = 0;
  NodeId target = 0;
};

/// Solver-neutral queries over terminal and derived CFL relations. Backends
/// may retain compressed results. Traversal order is unspecified and each
/// fact is visited once. Do not mutate the relation from a visitor.
class Relation {
public:
  virtual ~Relation() = default;

  virtual void ensureNodeCount(std::size_t node_count) = 0;
  virtual bool add(SymbolId symbol, NodeId source, NodeId target) = 0;
  virtual bool contains(SymbolId symbol, NodeId source,
                        NodeId target) const = 0;
  using NodeVisitor = llvm::function_ref<bool(NodeId)>;
  using EdgeVisitor = llvm::function_ref<bool(const RelationEdge &)>;
  // Return false when the visitor requests termination, true on completion.
  virtual bool visitSuccessors(SymbolId symbol, NodeId source,
                               NodeVisitor visitor) const = 0;
  virtual bool visitPredecessors(SymbolId symbol, NodeId target,
                                 NodeVisitor visitor) const = 0;
  virtual bool visitEdges(EdgeVisitor visitor) const = 0;
  virtual bool visitEdges(SymbolId symbol, EdgeVisitor visitor) const = 0;

  void forEachSuccessor(SymbolId symbol, NodeId source,
                        llvm::function_ref<void(NodeId)> visitor) const;
  void forEachPredecessor(SymbolId symbol, NodeId target,
                          llvm::function_ref<void(NodeId)> visitor) const;
  // Explicit collection allocates space proportional to the requested output.
  std::vector<RelationEdge> edges() const;
  std::vector<RelationEdge> edges(SymbolId symbol) const;
  virtual std::size_t edgeCount() const = 0;
  virtual std::size_t edgeCount(SymbolId symbol) const = 0;
  /// Estimated container payload only; excludes allocator and node overhead.
  virtual std::size_t estimatedPayloadBytes() const = 0;
};

enum class RelationBackend {
  SparseSets,
  SparseBitVectors,
};

std::unique_ptr<Relation> createRelation(RelationBackend backend,
                                         std::size_t node_count);

} // namespace lotus::cfl::classical

#pragma once

#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::detail {

class DisjointSets {
public:
  explicit DisjointSets(std::size_t count) : parent_(count), size_(count, 1) {
    std::iota(parent_.begin(), parent_.end(), std::size_t(0));
  }

  std::size_t find(std::size_t v) {
    std::size_t root = v;
    while (parent_.at(root) != root)
      root = parent_[root];
    while (parent_[v] != v) {
      const auto next = parent_[v];
      parent_[v] = root;
      v = next;
    }
    return root;
  }

  std::pair<std::size_t, std::size_t> join(std::size_t a, std::size_t b) {
    a = find(a);
    b = find(b);
    if (a == b)
      return {a, static_cast<std::size_t>(-1)};
    if (size_[a] < size_[b])
      std::swap(a, b);
    parent_[b] = a;
    size_[a] += size_[b];
    return {a, b};
  }

  // Consume the forest, returning dense component IDs without allocating a
  // second state-sized mapping. No find() calls may follow this operation.
  std::vector<std::size_t> takeComponents(std::size_t &count) {
    for (std::size_t v = 0; v < parent_.size(); ++v)
      parent_[v] = find(v);
    count = 0;
    for (std::size_t v = 0; v < parent_.size(); ++v)
      if (parent_[v] == v)
        size_[v] = count++;
    for (auto &root : parent_)
      root = size_[root];
    return std::move(parent_);
  }

  std::size_t payloadBytes() const {
    return (parent_.capacity() + size_.capacity()) * sizeof(std::size_t);
  }

private:
  std::vector<std::size_t> parent_;
  std::vector<std::size_t> size_;
};

} // namespace lotus::cfl::interleaved_dyck::detail

#include "CFL/InterleavedDyck/Core/BidirectedDyck.h"

#include "CFL/InterleavedDyck/Core/DisjointSets.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck {
namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

std::size_t checkedMultiply(std::size_t left, std::size_t right) {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    throw std::overflow_error("bidirected-Dyck edge table is too large");
  }
  return left * right;
}

using detail::DisjointSets;
using Clock = std::chrono::steady_clock;

std::uint64_t elapsed(Clock::time_point begin) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                               begin)
      .count();
}

struct LinkedList {
  std::size_t head = kNone;
  std::size_t tail = kNone;
  std::size_t size = 0;
};

class EdgePool {
public:
  void reserve(std::size_t count) {
    targets_.reserve(count);
    next_.reserve(count);
  }
  std::size_t size() const { return targets_.size(); }
  std::size_t payloadBytes() const {
    return (targets_.capacity() + next_.capacity()) * sizeof(std::size_t);
  }
  void append(LinkedList &list, std::size_t target) {
    const std::size_t node = targets_.size();
    targets_.push_back(target);
    next_.push_back(kNone);
    appendNode(list, node);
  }

  void splice(LinkedList &destination, LinkedList &source) {
    if (source.size == 0) {
      return;
    }
    if (destination.size == 0) {
      destination = source;
    } else {
      next_[destination.tail] = source.head;
      destination.tail = source.tail;
      destination.size += source.size;
    }
    source = {};
  }

  LinkedList detach(LinkedList &list) {
    const LinkedList result = list;
    list = {};
    return result;
  }

  void appendReusedSingleton(LinkedList &destination, const LinkedList &old,
                             std::size_t target) {
    if (old.head == kNone) {
      append(destination, target);
      return;
    }
    targets_[old.head] = target;
    next_[old.head] = kNone;
    LinkedList singleton{old.head, old.head, 1};
    splice(destination, singleton);
  }

  std::size_t target(std::size_t node) const { return targets_.at(node); }
  std::size_t next(std::size_t node) const { return next_.at(node); }

private:
  void appendNode(LinkedList &list, std::size_t node) {
    if (list.size == 0) {
      list = {node, node, 1};
      return;
    }
    next_[list.tail] = node;
    list.tail = node;
    ++list.size;
  }

  std::vector<std::size_t> targets_;
  std::vector<std::size_t> next_;
};

class SolverState {
public:
  SolverState(std::size_t state_count, std::size_t label_count,
              std::size_t closing_edge_hint)
      : sets_(state_count), label_count_(label_count),
        lists_(checkedMultiply(state_count, label_count)),
        queued_(lists_.size(), false), seen_(state_count, 0) {
    pool_.reserve(std::min(closing_edge_hint, lists_.size()));
  }

  void addClosing(std::size_t source, std::size_t target, std::size_t label) {
    auto &edges = list(source, label);
    // Generated transitions often repeat the same pair after epsilon
    // contraction. Avoid allocating a pool node for consecutive duplicates
    // within each source/label list.
    if (edges.tail == kNone || pool_.target(edges.tail) != target)
      pool_.append(edges, target);
  }

  std::size_t payloadBytes() const {
    return sets_.payloadBytes() + lists_.capacity() * sizeof(LinkedList) +
           queued_.capacity() + seen_.capacity() * sizeof(std::size_t) +
           pool_.payloadBytes() + targets_.capacity() * sizeof(std::size_t) +
           worklist_.size() * sizeof(std::pair<std::size_t, std::size_t>);
  }

  BidirectedDyckResult run(bool retain_quotient) {
    const std::size_t state_count = seen_.size();
    stats_.stored_closing_edges = pool_.size();
    recordPeak();
    for (std::size_t state = 0; state < state_count; ++state) {
      if (sets_.find(state) != state) {
        continue;
      }
      for (std::size_t label = 0; label < label_count_; ++label) {
        enqueue(state, label);
      }
    }

    recordPeak();
    while (!worklist_.empty()) {
      const auto [queued_state, label] = worklist_.front();
      worklist_.pop_front();
      ++stats_.worklist_pops;
      queued_[index(queued_state, label)] = false;

      const std::size_t source = sets_.find(queued_state);
      if (source != queued_state) {
        enqueue(source, label);
        continue;
      }

      LinkedList old = pool_.detach(list(source, label));
      targets_.clear();
      auto &targets = targets_;
      nextGeneration();
      for (std::size_t node = old.head; node != kNone;
           node = pool_.next(node)) {
        ++stats_.scanned_closing_edges;
        const std::size_t target = sets_.find(pool_.target(node));
        if (seen_[target] != generation_) {
          seen_[target] = generation_;
          targets.push_back(target);
        }
      }
      recordPeak();
      if (targets.empty()) {
        continue;
      }

      std::size_t target = targets.front();
      for (std::size_t i = 1; i < targets.size(); ++i) {
        target = joinWithLists(target, targets[i]);
      }
      target = sets_.find(target);
      const std::size_t current_source = sets_.find(source);
      pool_.appendReusedSingleton(list(current_source, label), old, target);
      enqueue(current_source, label);
      recordPeak();
    }

    BidirectedDyckResult result;
    result.component = sets_.takeComponents(stats_.components);
    if (retain_quotient) {
      result.quotient_closing_edges.reserve(std::min(
          pool_.size(), checkedMultiply(stats_.components, label_count_)));
      // Only root lists remain populated, with at most one target per label.
      // Reuse them rather than scanning the original dense graph again.
      for (std::size_t source = 0; source < state_count; ++source)
        for (std::size_t label = 0; label < label_count_; ++label) {
          const auto &edges = list(source, label);
          assert(edges.size <= 1);
          if (edges.size)
            result.quotient_closing_edges.push_back(
                {result.component[source],
                 result.component[pool_.target(edges.head)], label});
        }
      stats_.peak_working_bytes +=
          result.quotient_closing_edges.capacity() * sizeof(LabeledStateEdge);
    }
    result.stats = stats_;
    return result;
  }

private:
  void recordPeak() {
    stats_.peak_working_bytes =
        std::max(stats_.peak_working_bytes, payloadBytes());
  }

  std::size_t index(std::size_t state, std::size_t label) const {
    return state * label_count_ + label;
  }

  LinkedList &list(std::size_t state, std::size_t label) {
    return lists_[index(state, label)];
  }

  void enqueue(std::size_t state, std::size_t label) {
    state = sets_.find(state);
    const std::size_t slot = index(state, label);
    if (lists_[slot].size >= 2 && !queued_[slot]) {
      queued_[slot] = true;
      worklist_.emplace_back(state, label);
    }
  }

  std::size_t joinWithLists(std::size_t first, std::size_t second) {
    const auto [root, removed] = sets_.join(first, second);
    if (removed == kNone) {
      return root;
    }
    ++stats_.component_unions;
    for (std::size_t label = 0; label < label_count_; ++label) {
      pool_.splice(list(root, label), list(removed, label));
      enqueue(root, label);
    }
    return root;
  }

  void nextGeneration() {
    ++generation_;
    if (generation_ == 0) {
      std::fill(seen_.begin(), seen_.end(), 0);
      generation_ = 1;
    }
  }

  DisjointSets sets_;
  std::size_t label_count_ = 0;
  std::vector<LinkedList> lists_;
  std::vector<unsigned char> queued_;
  std::deque<std::pair<std::size_t, std::size_t>> worklist_;
  EdgePool pool_;
  std::vector<std::size_t> seen_;
  std::vector<std::size_t> targets_;
  std::size_t generation_ = 0;
  BidirectedDyckStats stats_;
};

} // namespace

BidirectedDyckResult BidirectedDyckComponentSolver::solveGenerated(
    std::size_t state_count, std::size_t label_count,
    const EpsilonSource &epsilon_edges, const ClosingSource &closing_edges,
    std::size_t closing_edge_hint, bool retain_quotient) const {
  if (state_count && !label_count)
    throw std::invalid_argument("bidirected-Dyck label count must be positive");
  BidirectedDyckStats stats;
  stats.states = state_count;
  auto begin = Clock::now();
  std::vector<std::size_t> mapping;
  {
    DisjointSets epsilon(state_count);
    stats.peak_working_bytes = epsilon.payloadBytes();
    epsilon_edges([&](std::size_t source, std::size_t target) {
      if (source >= state_count || target >= state_count)
        throw std::out_of_range("bidirected-Dyck epsilon edge state");
      ++stats.epsilon_edges;
      epsilon.join(source, target);
    });
    mapping = epsilon.takeComponents(stats.epsilon_components);
  }
  stats.epsilon_us = elapsed(begin);
  begin = Clock::now();
  SolverState solver(stats.epsilon_components, label_count, closing_edge_hint);
  closing_edges([&](std::size_t source, std::size_t target, std::size_t label) {
    if (source >= state_count || target >= state_count)
      throw std::out_of_range("bidirected-Dyck closing edge state");
    if (label >= label_count)
      throw std::out_of_range("bidirected-Dyck closing edge label");
    ++stats.closing_edges;
    solver.addClosing(mapping[source], mapping[target], label);
  });
  stats.closing_us = elapsed(begin);
  begin = Clock::now();
  auto compact = solver.run(retain_quotient);
  for (auto &component : mapping)
    component = compact.component[component];
  stats.components = compact.stats.components;
  stats.component_unions = compact.stats.component_unions;
  stats.worklist_pops = compact.stats.worklist_pops;
  stats.stored_closing_edges = compact.stats.stored_closing_edges;
  stats.scanned_closing_edges = compact.stats.scanned_closing_edges;
  stats.peak_working_bytes = std::max(stats.peak_working_bytes,
                                      mapping.capacity() * sizeof(std::size_t) +
                                          compact.stats.peak_working_bytes);
  stats.saturation_us = elapsed(begin);
  return {std::move(mapping), stats, std::move(compact.quotient_closing_edges)};
}

BidirectedDyckResult BidirectedDyckComponentSolver::solve(
    std::size_t state_count, std::size_t label_count,
    const std::vector<StatePair> &epsilon_edges,
    const std::vector<LabeledStateEdge> &closing_edges) const {
  return solveGenerated(
      state_count, label_count,
      [&](const EpsilonVisitor &visit) {
        for (const auto &edge : epsilon_edges)
          visit(edge.source, edge.target);
      },
      [&](const ClosingVisitor &visit) {
        for (const auto &edge : closing_edges)
          visit(edge.source, edge.target, edge.label);
      },
      closing_edges.size());
}

} // namespace lotus::cfl::interleaved_dyck

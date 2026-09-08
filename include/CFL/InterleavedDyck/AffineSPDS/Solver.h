#pragma once

#include "CFL/InterleavedDyck/AffineSPDS/Affine.h"
#include "CFL/InterleavedDyck/AffineSPDS/Observer.h"
#include "CFL/InterleavedDyck/SPDS/Solver.h"

#include <memory>

namespace lotus::cfl::interleaved_dyck::affine {

enum class Verdict { MayReach, ProjectionRejected, AffineSeparated };
enum class ComparisonMode { Joint, Independent, Projection };

class HistoryComparison {
public:
  HistoryComparison(AffineSpace call_history, AffineSpace field_history);
  bool mayReach() const;
  bool spdsMayReach() const { return !calls_.empty() && !fields_.empty(); }
  Verdict verdict() const;
  const AffineSpace &callHistory() const { return calls_; }
  const AffineSpace &fieldHistory() const { return fields_; }
  const std::optional<SeparationCertificate> &certificate() const;
  // Ablation: forget cross-block relationships AFTER computing the histories.
  // On block-diagonal observers this equals separate per-block analyses.
  bool independentMayReach(const HistoryObserver &observer) const;

private:
  AffineSpace calls_, fields_;
  mutable std::optional<Verdict> verdict_;
  mutable std::optional<std::optional<SeparationCertificate>> certificate_;
};

// Generic use with paired affine-weighted PDS automata. Both systems must use
// the SAME event interpretation; this is the caller's semantic responsibility.
HistoryComparison synchronize(const spds::Automaton<AffineSemiring> &calls,
                              const spds::Configuration &call_query,
                              const spds::Automaton<AffineSemiring> &fields,
                              const spds::Configuration &field_query);

struct Options {
  ObserverOptions observer;
  spds::StackAcceptance parentheses = spds::StackAcceptance::Empty;
  spds::StackAcceptance brackets = spds::StackAcceptance::Empty;
  spds::Limits
      limits; // per projection; failures throw, never return partial bounds
  std::size_t max_matrix_dimension = 0; // 0 = no explicit dimension cap
};
struct Statistics {
  spds::Statistics saturation;
  std::size_t matrix_dimension = 1;
  std::size_t maximum_affine_rank = 0;
};
struct Result {
  PairSet pairs;
  ComparisonMode mode = ComparisonMode::Joint;
  Statistics statistics;
  bool mayReach(Vertex source, Vertex target) const {
    return pairs.count({source, target}) != 0;
  }
};

class QueryResult {
public:
  Vertex anchor() const { return anchor_; }
  spds::Direction direction() const { return direction_; }
  const HistoryObserver &observer() const { return *observer_; }
  const spds::Automaton<AffineSemiring> &callAutomaton() const {
    return calls_;
  }
  const spds::Automaton<AffineSemiring> &fieldAutomaton() const {
    return fields_;
  }
  const Statistics &statistics() const { return statistics_; }
  // Lazy cache: const query methods are not safe for concurrent use of ONE
  // QueryResult. Distinct results/solvers share no mutable global state.
  const HistoryComparison &compare(Vertex vertex) const;
  bool mayReach(Vertex vertex) const { return compare(vertex).mayReach(); }
  bool spdsMayReach(Vertex vertex) const {
    return compare(vertex).spdsMayReach();
  }
  bool independentMayReach(Vertex vertex) const {
    return compare(vertex).independentMayReach(observer());
  }
  bool parenthesisReachable(Vertex vertex) const {
    return !compare(vertex).callHistory().empty();
  }
  bool bracketReachable(Vertex vertex) const {
    return !compare(vertex).fieldHistory().empty();
  }
  // Post: exact endpoint stacks; Pre: exact predecessor stacks. Top first,
  // ordinary unsigned label IDs; callers do not add the protected bottom.
  HistoryComparison compareStacks(Vertex vertex,
                                  const std::vector<unsigned> &calls,
                                  const std::vector<unsigned> &fields) const;
  bool mayAccept(Vertex vertex, const std::vector<unsigned> &calls,
                 const std::vector<unsigned> &fields) const {
    return compareStacks(vertex, calls, fields).mayReach();
  }

private:
  friend class PreparedAnalysis;
  QueryResult(Vertex anchor, spds::Direction direction, Options options,
              std::shared_ptr<const std::map<Vertex, spds::State>> controls,
              std::shared_ptr<const HistoryObserver> observer,
              spds::Automaton<AffineSemiring> calls,
              spds::Automaton<AffineSemiring> fields);
  AffineSpace history(const spds::Automaton<AffineSemiring> &automaton,
                      Vertex vertex, spds::StackAcceptance acceptance) const;
  Vertex anchor_;
  spds::Direction direction_;
  Options options_;
  std::shared_ptr<const std::map<Vertex, spds::State>> controls_;
  std::shared_ptr<const HistoryObserver> observer_;
  spds::Automaton<AffineSemiring> calls_, fields_;
  Statistics statistics_;
  mutable std::map<Vertex, HistoryComparison> cache_;
};

// Fixed computational algebra, no SAT/observer synthesis. Computes exact
// affine hulls for EACH projected witness language under the given observer.
// Their intersection is a sound upper bound, NOT a concrete two-stack witness.
class PreparedAnalysis;
class Solver {
public:
  explicit Solver(Options options = {}) : options_(options) {}
  PreparedAnalysis prepare(const Graph &graph) const;
  PreparedAnalysis prepare(const Graph &graph,
                           const HistoryObserver &observer) const;

private:
  void validate(const Graph &graph, const HistoryObserver &observer) const;
  Options options_;
};

class PreparedAnalysis {
public:
  QueryResult queryFrom(Vertex source) const;
  QueryResult queryTo(Vertex target) const;
  Result analyzeAll(ComparisonMode mode = ComparisonMode::Joint) const;
  Result analyzeDemands(
      const std::vector<Pair> &demands,
      ComparisonMode mode = ComparisonMode::Joint,
      spds::DemandDirection direction = spds::DemandDirection::Auto) const;
  const HistoryObserver &observer() const { return *observer_; }

private:
  friend class Solver;
  PreparedAnalysis(Options options, std::map<Vertex, spds::State> controls,
                   std::shared_ptr<const HistoryObserver> observer,
                   spds::PushdownSystem<AffineSemiring> calls,
                   spds::PushdownSystem<AffineSemiring> fields,
                   std::optional<spds::PreparedAnalysis> boolean);
  QueryResult query(Vertex anchor, spds::Direction direction) const;
  Options options_;
  std::shared_ptr<const std::map<Vertex, spds::State>> controls_;
  std::shared_ptr<const HistoryObserver> observer_;
  spds::PushdownSystem<AffineSemiring> calls_, fields_;
  std::optional<spds::PreparedAnalysis> boolean_;
};

} // namespace lotus::cfl::interleaved_dyck::affine

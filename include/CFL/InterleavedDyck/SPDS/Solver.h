#pragma once

#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/SPDS/Pushdown.h"

namespace lotus::cfl::interleaved_dyck::spds {

enum class StackAcceptance { Empty, Any };
struct Options {
  // Empty/Empty is the balanced interleaved-Dyck upper bound. Any allows
  // unmatched opens at the endpoint, but NEVER an underflow or mismatched pop.
  StackAcceptance parentheses = StackAcceptance::Empty;
  StackAcceptance brackets = StackAcceptance::Empty;
  Limits limits; // per individual PDS saturation; no partial results on failure
};
struct Result {
  PairSet upper_bound;
  PairSet parenthesis_pairs;
  PairSet bracket_pairs;
  Statistics statistics; // totals over individual projection runs
  bool mayReach(Vertex from, Vertex to) const {
    return upper_bound.count({from, to}) != 0;
  }
};

class QueryResult {
public:
  Vertex anchor() const { return anchor_; }
  Direction direction() const { return direction_; }
  // Post: anchor -> vertex. Pre: vertex -> anchor. The anchor's stacks are
  // empty; Options controls acceptance at the queried vertex in either case.
  bool mayReach(Vertex vertex) const;
  bool parenthesisReachable(Vertex vertex) const;
  bool bracketReachable(Vertex vertex) const;
  // For post*, query a precise pair of endpoint stacks. For pre*, query
  // precise predecessor stacks. Both vectors are top first; no bottom needed.
  bool mayAccept(Vertex vertex, const std::vector<unsigned> &parentheses,
                 const std::vector<unsigned> &brackets) const;
  const Automaton<BooleanSemiring> &callAutomaton() const { return calls_; }
  const Automaton<BooleanSemiring> &fieldAutomaton() const { return fields_; }
private:
  friend class Solver;
  QueryResult(Vertex anchor, Direction direction, Options options,
              std::map<Vertex, State> controls, Automaton<BooleanSemiring> calls,
              Automaton<BooleanSemiring> fields);
  bool reachable(const Automaton<BooleanSemiring> &automaton, Vertex vertex,
                 StackAcceptance acceptance) const;
  Vertex anchor_;
  Direction direction_;
  Options options_;
  std::map<Vertex, State> controls_;
  Automaton<BooleanSemiring> calls_, fields_;
};

// Graph specialization of Definition 4 (POPL 2019, Spath/Ali/Bodden).
// Each projection is exact. Their conjunction is a sound upper bound, NOT
// an exact same-path two-stack reachability procedure.
class Solver {
public:
  explicit Solver(Options options = {}) : options_(options) {}
  Result analyze(const Graph &graph) const;
  QueryResult analyzeFrom(const Graph &graph, Vertex source) const;
  QueryResult analyzeTo(const Graph &graph, Vertex target) const;
private:
  QueryResult query(const Graph &graph, Vertex anchor, Direction direction) const;
  Options options_;
};

} // namespace lotus::cfl::interleaved_dyck::spds

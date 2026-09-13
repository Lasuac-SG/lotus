#ifndef DATAFLOW_APA_ENGINES_STATEELIMINATIONSOLVER_H_
#define DATAFLOW_APA_ENGINES_STATEELIMINATIONSOLVER_H_

#include "Dataflow/APA/EAN/DagStats.h"
#include "Dataflow/APA/Solver/EliminationOrder.h"
#include "Dataflow/APA/Solver/SolverContext.h"

#include <chrono>

namespace elimination {
namespace detail {

// Build the boolean elimination graph from the current matrix's off-diagonal
// nonzeros (direct CFG edges at this point). Self-loops are the diagonal
// (always one()) and are intentionally excluded — they do not affect the
// predecessor–successor product.
template <typename AnalysisDomainTy>
order::EliminationGraph buildEliminationGraph(
    const IntraEliminationSolverContext<AnalysisDomainTy> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisDomainTy>;
  const auto N = Ctx.Nodes.size();
  order::EliminationGraph G(N);
  for (std::size_t i = 0; i < N; ++i) {
    for (std::size_t j = 0; j < N; ++j) {
      if (i == j) {
        continue;
      }
      if (!Context::expr_factory_t::isZero(Ctx.Matrix[i][j])) {
        G.addEdge(i, j);
      }
    }
  }
  return G;
}

// Generic Floyd-Warshall-style elimination over the full CFG. This engine
// makes no reducibility assumptions and therefore serves as the baseline
// implementation as well as the fallback when ADT-specific preconditions fail.
template <typename AnalysisTypesT>
std::vector<std::size_t> getStateEliminationOrder(
    const IntraEliminationSolverContext<AnalysisTypesT> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisTypesT>;
  const auto N = Ctx.Nodes.size();

  // Cost-aware policy: greedy minimum-product order over the elimination graph.
  // Fully replaces the baseline order (including the reducible reverse-topo
  // path). The final all-pairs result is invariant to pivot order.
  if (Ctx.Opts.Ordering == OrderingPolicy::CostAware) {
    return order::computeCostAwareOrder(buildEliminationGraph(Ctx));
  }

  std::vector<std::size_t> Order(N);
  const auto *R =
      dynamic_cast<const typename Context::ReducibleProblemTy *>(&Ctx.Problem);
  if (R != nullptr) {
    const auto Topo = R->topologicalOrder();
    if (Topo.size() == N) {
      for (std::size_t i = 0; i < N; ++i) {
        const auto It = Ctx.Index.find(Topo[N - 1 - i]);
        if (It == Ctx.Index.end()) {
          goto DefaultOrder;
        }
        Order[i] = It->second;
      }
      return Order;
    }
  }
DefaultOrder:
  for (std::size_t i = 0; i < N; ++i) {
    Order[i] = i;
  }
  return Order;
}

template <typename AnalysisTypesT>
void buildStateEliminationMatrix(
    IntraEliminationSolverContext<AnalysisTypesT> &Ctx) {
  // Build the usual elimination matrix where M[i][j] summarizes all direct
  // edges from node i to node j. Diagonals start at one() so paths are allowed
  // to stay at a node before additional eliminations introduce loops.
  Ctx.Nodes = Ctx.Problem.nodes();
  Ctx.Index.clear();
  Ctx.Index.reserve(Ctx.Nodes.size());
  for (std::size_t i = 0; i < Ctx.Nodes.size(); ++i) {
    Ctx.Index.emplace(Ctx.Nodes[i], i);
  }

  const auto N = Ctx.Nodes.size();
  Ctx.Matrix.assign(
      N,
      std::vector<
          typename IntraEliminationSolverContext<AnalysisTypesT>::expr_ref_t>(
          N, Ctx.Exprs.zero()));
  for (std::size_t i = 0; i < N; ++i) {
    Ctx.Matrix[i][i] = Ctx.Exprs.one();
  }

  for (const auto &Src : Ctx.Nodes) {
    const auto SrcIdx = Ctx.idx(Src);
    for (const auto &Dst : Ctx.Problem.succs(Src)) {
      const auto It = Ctx.Index.find(Dst);
      if (It == Ctx.Index.end()) {
        continue;
      }
      const auto DstIdx = It->second;
      Ctx.Matrix[SrcIdx][DstIdx] =
          Ctx.Exprs.unite(Ctx.Matrix[SrcIdx][DstIdx],
                          Ctx.Exprs.atom(Ctx.Problem.edgeTransfer(Src, Dst)));
    }
  }
}

template <typename AnalysisTypesT>
void eliminateStateIntermediates(
    IntraEliminationSolverContext<AnalysisTypesT> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisTypesT>;
  const auto N = Ctx.Nodes.size();
  std::vector<typename Context::expr_ref_t> ColK(N);
  std::vector<typename Context::expr_ref_t> RowK(N);

  // Opt-in RQ3 instrumentation: peak unique DAG nodes across the whole matrix.
  const bool Measure = Ctx.Opts.MeasurePeakNodes;
  auto measurePeak = [&]() {
    if (!Measure) {
      return;
    }
    std::vector<typename Context::expr_ref_t> Live;
    Live.reserve(N * N);
    for (std::size_t i = 0; i < N; ++i) {
      for (std::size_t j = 0; j < N; ++j) {
        if (!Context::expr_factory_t::isZero(Ctx.Matrix[i][j])) {
          Live.push_back(Ctx.Matrix[i][j]);
        }
      }
    }
    const std::size_t nodes = ean::computeDagStats<transfer_t>(Live).uniqueNodes;
    if (nodes > Ctx.Diagnostics.peak_matrix_nodes) {
      Ctx.Diagnostics.peak_matrix_nodes = nodes;
    }
  };

  const auto Order = getStateEliminationOrder(Ctx);
  measurePeak(); // initial (direct-edge) matrix
  for (std::size_t ki = 0; ki < N; ++ki) {
    const std::size_t k = Order[ki];
    // Snapshot row/column k before mutating the matrix. This mirrors the
    // standard state-elimination update:
    //   M[i][j] |= M[i][k] . M[k][k]* . M[k][j]
    for (std::size_t i = 0; i < N; ++i) {
      ColK[i] = Ctx.Matrix[i][k];
    }
    for (std::size_t j = 0; j < N; ++j) {
      RowK[j] = Ctx.Matrix[k][j];
    }

    const auto KStar = Ctx.Exprs.star(Ctx.Matrix[k][k]);
    for (std::size_t i = 0; i < N; ++i) {
      if (Context::expr_factory_t::isZero(ColK[i])) {
        continue;
      }
      for (std::size_t j = 0; j < N; ++j) {
        if (Context::expr_factory_t::isZero(RowK[j])) {
          continue;
        }
        auto Via = Ctx.Exprs.concat(ColK[i], KStar);
        Via = Ctx.Exprs.concat(Via, RowK[j]);
        Ctx.Matrix[i][j] = Ctx.Exprs.unite(Ctx.Matrix[i][j], Via);
      }
    }
    measurePeak(); // after eliminating k
  }
}

template <typename AnalysisTypesT>
bool materializeStateResults(
    IntraEliminationSolverContext<AnalysisTypesT> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisTypesT>;
  Ctx.Results = typename Context::result_t{};
  if (Ctx.Nodes.empty()) {
    return true;
  }

  const auto EntryIt = Ctx.Index.find(Ctx.Problem.entry());
  if (EntryIt == Ctx.Index.end()) {
    return false;
  }
  const auto EntryIdx = EntryIt->second;

  const auto Init = Ctx.Problem.initialFact();
  const std::size_t Reps = Ctx.Opts.InterpRepeat ? Ctx.Opts.InterpRepeat : 1;
  const auto InterpStart = std::chrono::steady_clock::now();
  for (std::size_t j = 0; j < Ctx.Nodes.size(); ++j) {
    const auto &N = Ctx.Nodes[j];
    // Each remaining matrix entry summarizes all paths from entry to N.
    auto E = Ctx.Matrix[EntryIdx][j];
    Ctx.Results.ExprTo(N) = E;
    // Skip the interpretation when EAN or Greedy will re-optimize and
    // re-evaluate the whole batch afterwards (avoids a wasted eval), or when a
    // memoizing client interpreter (InterpMemo) will fill IN facts itself.
    if (!Ctx.Opts.EnableEAN && !Ctx.Opts.EnableGreedy && !Ctx.Opts.InterpMemo) {
      typename Context::fact_t V = Ctx.eval(E, Init);
      for (std::size_t r = 1; r < Reps; ++r) {
        V = Ctx.eval(E, Init); // amortization measurement (RQ2)
      }
      Ctx.Results.IN(N) = std::move(V);
    }
  }
  Ctx.Diagnostics.interp_time_us += static_cast<std::size_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - InterpStart)
          .count());
  return true;
}

template <typename AnalysisTypesT>
bool solveStateElimination(
    IntraEliminationSolverContext<AnalysisTypesT> &Ctx) {
  const auto GenStart = std::chrono::steady_clock::now();
  buildStateEliminationMatrix(Ctx);
  eliminateStateIntermediates(Ctx);
  Ctx.Diagnostics.gen_time_us += static_cast<std::size_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - GenStart)
          .count());
  return materializeStateResults(Ctx);
}

} // namespace detail
} // namespace elimination

#endif // DATAFLOW_APA_ENGINES_STATEELIMINATIONSOLVER_H_

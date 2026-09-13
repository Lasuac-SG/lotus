#pragma once

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include "Dataflow/NPA/LLVM/AnalysisSupport.h"
#include "Dataflow/NPA/LLVM/ForwardInterEngine.h"
#include "Dataflow/NPA/NPA.h"

#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace npa {

template <class D, class Analysis> class IntraEngine {
public:
  using Exp = Exp0<D>;
  using E = E0<D>;
  using Val = typename D::value_type;
  using Fact = typename Analysis::FactType;

  struct Result {
    AnalysisStatus status;
    Val summary = D::zero();
    std::map<BlockKey, Fact> blockEntryFacts;
    std::map<BlockKey, Fact> blockExitFacts;
  };

  static Result
  run(llvm::Function &F, Analysis &analysis, bool verbose = false,
      LinearStrategy linearStrategy = LinearStrategy::SCC,
      NewtonRoundStrategy roundStrategy = NewtonRoundStrategy::Dense) {
    Result result;
    std::vector<std::pair<Symbol, E>> equations;
    std::unordered_map<std::string, E> entryExpressions;
    std::unordered_map<std::string, E> exitExpressions;
    std::unordered_map<const llvm::BasicBlock *, E> blockBodies;

    for (auto &BB : F) {
      E current = Exp::term(D::one());
      for (auto &I : BB) {
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
          current =
              Exp::seq(getCallToReturnTransfer(analysis, *Call, 0), current);
        }
        current = analysis.getTransfer(I, current);
      }
      blockBodies.emplace(&BB, std::move(current));
    }

    E functionSummary;
    for (auto &BB : F) {
      E incoming;
      if (&BB == &F.getEntryBlock())
        incoming = Exp::term(D::one());
      for (auto *Pred : llvm::predecessors(&BB)) {
        auto *Term = Pred->getTerminator();
        Val edge = Term != nullptr ? getEdgeTransfer(analysis, *Term, BB, 0)
                                   : D::one();
        E branch =
            Exp::seq(std::move(edge),
                     Exp::hole(InterEngine<D, Analysis>::getBlockSymbol(Pred)));
        incoming = combine(std::move(incoming), std::move(branch));
      }
      if (!incoming)
        incoming = Exp::term(D::zero());

      E entry = buildBlockEntryExpr(analysis, BB, std::move(incoming), 0);
      const auto symbol = InterEngine<D, Analysis>::getBlockSymbol(&BB);
      E exit = multiply(blockBodies.at(&BB), entry);
      equations.emplace_back(symbol, exit);
      entryExpressions.emplace(symbol, entry);
      exitExpressions.emplace(symbol, Exp::hole(symbol));

      auto *Term = BB.getTerminator();
      if (Term == nullptr || Term->getNumSuccessors() == 0)
        functionSummary =
            combine(std::move(functionSummary), Exp::hole(symbol));
    }

    if (!functionSummary)
      functionSummary = Exp::term(D::zero());

    auto solved = NPASolver<D>::solve(
        equations, verbose, -1, linearStrategy, DomainContractMode::Off,
        ConvergencePolicy::DomainDefault, roundStrategy);
    result.status.summary_solve = solved.second;
    result.status.used_bounded_inner_solve =
        solved.second.hit_linear_limit || solved.second.hit_fixpoint_limit;
    result.status.approximated =
        !solved.second.converged || result.status.used_bounded_inner_solve;
    result.status.overall_converged = solved.second.converged;
    result.status.overall_hit_limit = result.status.used_bounded_inner_solve;

    std::unordered_map<Symbol, Val> environment;
    for (auto &Entry : solved.first)
      environment.insert_or_assign(Entry.first, Entry.second);

    typename I0<D>::EvaluationContext context;
    result.summary =
        I0<D>::evalCachedWithContext(environment, {}, functionSummary, context);
    const Fact initial = analysis.getEntryValue();
    for (auto &BB : F) {
      const auto symbol = InterEngine<D, Analysis>::getBlockSymbol(&BB);
      auto EntryIt = entryExpressions.find(symbol);
      auto ExitIt = exitExpressions.find(symbol);
      if (EntryIt != entryExpressions.end()) {
        auto summary = I0<D>::evalCachedWithContext(environment, {},
                                                    EntryIt->second, context);
        result.blockEntryFacts[{&BB}] = analysis.applySummary(summary, initial);
      }
      if (ExitIt != exitExpressions.end()) {
        auto summary = I0<D>::evalCachedWithContext(environment, {},
                                                    ExitIt->second, context);
        result.blockExitFacts[{&BB}] = analysis.applySummary(summary, initial);
      }
    }
    return result;
  }

private:
  template <typename A>
  static auto getCallToReturnTransfer(A &analysis, const llvm::CallBase &Call,
                                      int)
      -> decltype(analysis.getCallToReturnTransfer(Call)) {
    return analysis.getCallToReturnTransfer(Call);
  }

  static Val getCallToReturnTransfer(Analysis &, const llvm::CallBase &, long) {
    return D::one();
  }

  template <typename A>
  static auto getEdgeTransfer(A &analysis, const llvm::Instruction &Term,
                              const llvm::BasicBlock &Succ, int)
      -> decltype(analysis.getEdgeTransfer(Term, Succ)) {
    return analysis.getEdgeTransfer(Term, Succ);
  }

  static Val getEdgeTransfer(Analysis &, const llvm::Instruction &,
                             const llvm::BasicBlock &, long) {
    return D::one();
  }

  template <typename A>
  static auto buildBlockEntryExpr(A &analysis, llvm::BasicBlock &BB, E Expr,
                                  int)
      -> decltype(analysis.buildBlockEntryExpr(BB, Expr)) {
    return analysis.buildBlockEntryExpr(BB, std::move(Expr));
  }

  static E buildBlockEntryExpr(Analysis &, llvm::BasicBlock &, E Expr, long) {
    return Expr;
  }

  static bool isZero(const E &Expr) {
    return Expr && Expr->k == Exp::Term && D::equal(Expr->c, D::zero());
  }

  static bool isOne(const E &Expr) {
    return Expr && Expr->k == Exp::Term && D::equal(Expr->c, D::one());
  }

  static E combine(E Lhs, E Rhs) {
    if (!Lhs)
      return Rhs;
    if (!Rhs)
      return Lhs;
    if (isZero(Lhs))
      return Rhs;
    if (isZero(Rhs))
      return Lhs;
    return Exp::ndet(Lhs, Rhs);
  }

  static E multiply(E Lhs, E Rhs) {
    if (!Lhs || !Rhs || isZero(Lhs) || isZero(Rhs))
      return Exp::term(D::zero());
    if (isOne(Lhs))
      return Rhs;
    if (isOne(Rhs))
      return Lhs;
    if (Lhs->k == Exp::Term && Rhs->k == Exp::Term)
      return Exp::term(D::extend(Lhs->c, Rhs->c));
    if (Lhs->k == Exp::Term)
      return Exp::seq(Lhs->c, Rhs);
    return Exp::mul(Lhs, Rhs);
  }
};

} // namespace npa

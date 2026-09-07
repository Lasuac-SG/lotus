/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Inter/ReachingDefinitions.h"

#include "Dataflow/NPA/LLVM/ForwardInterEngine.h"

#include <llvm/IR/Instructions.h>

namespace npa {

// Analysis Policy for Reaching Definitions (Gen/Kill)
class RDAnalysis {
public:
  using FactType = GenKillTransformer::fact_type;

private:
  using D = GenKillTransformer;
  using Exp = Exp0<D>;
  using E = E0<D>;

  std::unordered_map<const llvm::Value *, unsigned> valToBit;

public:
  RDAnalysis(llvm::Module &M) {
    unsigned bit = 0;
    for (auto &F : M) {
      for (auto &Arg : F.args())
        valToBit[&Arg] = bit++;
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (!I.getType()->isVoidTy())
            valToBit[&I] = bit++;
        }
      }
    }
  }

  FactType getEntryValue() const { return {}; }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (valToBit.count(&I)) {
      return Exp::seq(D::generate(valToBit[&I]), currentPath);
    }
    return currentPath;
  }

  FactType applySummary(const D::value_type &summary, const FactType &fact) {
    return D::apply(summary, fact);
  }

  FactType joinFacts(const FactType &a, const FactType &b) {
    FactType result = a;
    result |= b;
    return result;
  }

  bool factsEqual(const FactType &a, const FactType &b) { return a == b; }
};

InterReachingDefinitions::Result
InterReachingDefinitions::run(llvm::Module &M, bool verbose,
                              LinearStrategy linearStrategy,
                              IndirectCallResolutionMode callResolutionMode,
                              NewtonRoundStrategy roundStrategy) {
  RDAnalysis analysis(M);
  auto engineResult = InterEngine<GenKillTransformer, RDAnalysis>::run(
      M, analysis, verbose, linearStrategy, callResolutionMode, roundStrategy);

  InterReachingDefinitions::Result res;
  res.status = engineResult.status;
  res.summaries.insert(engineResult.summaries.begin(),
                       engineResult.summaries.end());
  res.blockFacts = std::move(engineResult.blockEntryFacts);
  return res;
}

} // namespace npa

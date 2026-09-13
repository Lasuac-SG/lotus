#include "Dataflow/NPA/Analyses/Inter/ReachableBlocks.h"

#include "Dataflow/NPA/LLVM/ForwardInterEngine.h"

namespace npa {
namespace {

class ReachableBlocksAnalysis {
public:
  using FactType = GenKillTransformer::fact_type;
  using D = GenKillTransformer;
  using Exp = Exp0<D>;
  using E = E0<D>;

  FactType getEntryValue() const {
    FactType Result;
    Result.set(0);
    return Result;
  }

  E getTransfer(llvm::Instruction &, E CurrentPath) { return CurrentPath; }

  FactType applySummary(const D::value_type &Summary,
                        const FactType &Fact) const {
    return D::apply(Summary, Fact);
  }

  FactType joinFacts(const FactType &Lhs, const FactType &Rhs) const {
    FactType Result = Lhs;
    Result |= Rhs;
    return Result;
  }

  bool factsEqual(const FactType &Lhs, const FactType &Rhs) const {
    return Lhs == Rhs;
  }
};

} // namespace

InterReachableBlocks::Result
InterReachableBlocks::run(llvm::Module &M, bool verbose,
                          LinearStrategy linearStrategy,
                          IndirectCallResolutionMode callResolutionMode,
                          NewtonRoundStrategy roundStrategy) {
  ReachableBlocksAnalysis Analysis;
  auto EngineResult =
      InterEngine<GenKillTransformer, ReachableBlocksAnalysis>::run(
          M, Analysis, verbose, linearStrategy, callResolutionMode,
          roundStrategy);

  Result Result;
  Result.status = EngineResult.status;
  Result.summaries = std::move(EngineResult.summaries);
  for (const auto &Entry : EngineResult.blockEntryFacts) {
    if (Entry.second.test(0))
      Result.reachableBlocks.insert(Entry.first.block);
  }
  return Result;
}

} // namespace npa

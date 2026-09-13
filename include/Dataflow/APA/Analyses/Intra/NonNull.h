#pragma once

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/NonNullDomain.h"

namespace elimination {

using NonNullResult =
    DataFlowResultT<llvm::Instruction *, NonNullFact, NonNullEdgeTransfer>;

NonNullResult runIntraElimNonNull(llvm::Function *F,
                                  EliminationOptions Opts = {});

NonNullResult runIntraElimNonNull(llvm::Function *F, llvm::AssumptionCache *AC,
                                  llvm::DominatorTree *DT,
                                  EliminationOptions Opts = {});

} // namespace elimination

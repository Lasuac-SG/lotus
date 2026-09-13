#pragma once

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/UninitializedVariablesDomain.h"
#include "Dataflow/APA/LLVM/ForwardProblem.h"

namespace elimination {

using UninitializedVariablesResult =
    DataFlowResultT<llvm::Instruction *, UninitializedVariablesFact,
                    llvm::Instruction *>;

UninitializedVariablesResult
runIntraElimUninitializedVariables(llvm::Function *F,
                                   EliminationOptions Opts = {});

UninitializedVariablesResult
runIntraElimUninitializedVariables(llvm::Function *F, llvm::AAResults *AA,
                                   EliminationOptions Opts = {});

UninitializedVariablesResult runIntraElimUninitializedVariables(
    llvm::Function *F, llvm::AAResults *AA, llvm::AssumptionCache *AC,
    llvm::DominatorTree *DT, EliminationOptions Opts = {});

} // namespace elimination

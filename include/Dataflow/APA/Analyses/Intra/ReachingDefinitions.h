#pragma once

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/ReachingDefinitionsDomain.h"
#include "Dataflow/APA/LLVM/ForwardProblem.h"

namespace elimination {

using ReachingDefinitionsResult =
    DataFlowResultT<llvm::Instruction *, ReachingDefinitionsFact,
                    llvm::Instruction *>;

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F,
                                EliminationOptions Opts = {});

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, llvm::AAResults *AA,
                                EliminationOptions Opts = {});

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, llvm::AAResults *AA,
                                llvm::MemorySSA *MSSA,
                                EliminationOptions Opts = {});

// TranslAPA baseline: same front-end (path-expression DAG), but interpret each
// summary with the closed-form Gen/Kill semiring (paper §4) instead of the
// generic fixpoint interpreter. Records the fold time in
// SolveDiagnostics::interp_time_us. Reaching definitions is a separable
// Gen/Kill problem, so the mechanically-translated result equals the generic
// result.
ReachingDefinitionsResult
runIntraTranslApaReachingDefinitions(llvm::Function *F,
                                     llvm::AAResults *AA = nullptr,
                                     EliminationOptions Opts = {});

} // namespace elimination

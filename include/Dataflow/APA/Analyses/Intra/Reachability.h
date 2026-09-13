#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/ReachabilityDomain.h"
#include "Dataflow/APA/LLVM/ForwardProblem.h"

namespace elimination {

using ReachabilityResult =
    DataFlowResultT<llvm::Instruction *, ReachableFact, llvm::Instruction *>;

ReachabilityResult runIntraElimReachability(llvm::Function *F,
                                            EliminationOptions Opts = {});

// TranslAPA baseline variant: interpret the path-expression DAG with the
// closed-form Gen/Kill semiring. Reachability is the degenerate one-fact
// lattice (identity transfer), so this is mainly a wiring/timing smoke path;
// the fold time is recorded in SolveDiagnostics::interp_time_us.
ReachabilityResult runIntraTranslApaReachability(llvm::Function *F,
                                                 EliminationOptions Opts = {});

} // namespace elimination

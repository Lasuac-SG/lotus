#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_REACHABILITY_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_REACHABILITY_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Adapters/LLVM/ForwardProblem.h"

namespace elimination {

using ReachableFact = bool;
using ReachableResult =
    DataFlowResultT<llvm::Instruction *, ReachableFact, llvm::Instruction *>;

ReachableResult runIntraElimReachable(llvm::Function *F,
                                      EliminationOptions Opts = {});

// TranslAPA baseline variant: interpret the path-expression DAG with the
// closed-form Gen/Kill semiring. Reachability is the degenerate one-fact
// lattice (identity transfer), so this is mainly a wiring/timing smoke path;
// the fold time is recorded in SolveDiagnostics::interp_time_us.
ReachableResult runIntraTranslApaReachable(llvm::Function *F,
                                           EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_REACHABILITY_H_

#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/SignDomain.h"
#include "Dataflow/APA/LLVM/ForwardProblem.h"

namespace elimination {

using SignResult =
    DataFlowResultT<llvm::Instruction *, SignMap, llvm::Instruction *>;

SignResult runIntraElimSign(llvm::Function *F, EliminationOptions Opts = {});

} // namespace elimination

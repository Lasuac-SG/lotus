#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_AFFINE_EQUALITIES_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_AFFINE_EQUALITIES_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Adapters/LLVM/ForwardProblem.h"
#include "Dataflow/APA/Domains/AffineRelationDomain.h"

namespace elimination {

// Intraprocedural affine-relation (affine-equalities) analysis on the
// path-expression elimination solver. The fact is an AffineRelation over the
// function's tracked scalar vocabulary; the path-expression interpreter maps
// Union -> combine (affine hull / join), Concat -> extend (relational
// composition), Star -> fixpoint iteration (converges finitely in Howell normal
// form). Because the affine transformer distributes over the join, the full
// Kleene EAN law profile is sound for this client (unlike reachability /
// liveness), making it the paper's positive R1 (law-gated admissibility) case.
using AffineFact = AffineRelationDomain::value_type;
using AffineResult =
    DataFlowResultT<llvm::Instruction *, AffineFact, llvm::Instruction *>;

AffineResult runIntraElimAffine(llvm::Function *F, EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_AFFINE_EQUALITIES_H_

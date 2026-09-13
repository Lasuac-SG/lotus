#pragma once

#include "Dataflow/APA/Domains/IndexedSet.h"

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

struct UninitializedVariablesDomain : IndexedUnionDomain<llvm::Value *> {};

using UninitializedVariablesFact = UninitializedVariablesDomain::value_type;

} // namespace elimination

#pragma once

#include "Dataflow/APA/Domains/IndexedSet.h"

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

struct LiveVariablesDomain : IndexedUnionDomain<const llvm::Value *> {};

using LiveVariablesFact = LiveVariablesDomain::value_type;

} // namespace elimination

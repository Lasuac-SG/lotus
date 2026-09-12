#pragma once

#include "Dataflow/APA/Domains/IndexedSet.h"

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

struct LocksetDomain : IndexedUnionDomain<const llvm::Value *> {};

using LocksetFact = LocksetDomain::value_type;

} // namespace elimination

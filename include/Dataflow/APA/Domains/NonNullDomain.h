#pragma once

#include "Dataflow/APA/Domains/IndexedSet.h"

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

struct NonNullDomain : IndexedIntersectionDomain<const llvm::Value *> {
  using IndexedIntersectionDomain::IndexedIntersectionDomain;
};

using NonNullFact = NonNullDomain::value_type;

} // namespace elimination

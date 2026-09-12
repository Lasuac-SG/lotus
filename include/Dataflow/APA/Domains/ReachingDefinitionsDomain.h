#pragma once

#include "Dataflow/APA/Domains/IndexedSet.h"

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

struct ReachingDefinitionsDomain : IndexedUnionDomain<const llvm::Value *> {};

using ReachingDefinitionsFact = ReachingDefinitionsDomain::value_type;

} // namespace elimination

#pragma once

#include "Dataflow/APA/Domains/ExpressionKey.h"
#include "Dataflow/APA/Domains/IndexedSet.h"

namespace elimination {

struct VeryBusyExpressionsDomain : IndexedIntersectionDomain<ExpressionKey> {
  using IndexedIntersectionDomain::IndexedIntersectionDomain;
};

using VeryBusyExpressionsFact = VeryBusyExpressionsDomain::value_type;

} // namespace elimination

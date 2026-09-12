#pragma once

#include "Dataflow/APA/Domains/ExpressionKey.h"
#include "Dataflow/APA/Domains/IndexedSet.h"

namespace elimination {

struct AvailableExpressionsDomain : IndexedIntersectionDomain<ExpressionKey> {
  using IndexedIntersectionDomain::IndexedIntersectionDomain;
};

using AvailableExpressionsFact = AvailableExpressionsDomain::value_type;

} // namespace elimination

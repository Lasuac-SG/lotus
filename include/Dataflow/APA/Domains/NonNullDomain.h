#pragma once

#include "Dataflow/APA/Domains/IndexedSet.h"

#include <cstddef>
#include <functional>

namespace llvm {
class Instruction;
class Value;
} // namespace llvm

namespace elimination {

struct NonNullDomain : IndexedIntersectionDomain<const llvm::Value *> {
  using IndexedIntersectionDomain::IndexedIntersectionDomain;
};

using NonNullFact = NonNullDomain::value_type;

struct NonNullEdgeTransfer {
  llvm::Instruction *Src = nullptr;
  llvm::Instruction *Dst = nullptr;

  friend bool operator==(const NonNullEdgeTransfer &Lhs,
                         const NonNullEdgeTransfer &Rhs) {
    return Lhs.Src == Rhs.Src && Lhs.Dst == Rhs.Dst;
  }
};

} // namespace elimination

namespace std {
template <> struct hash<elimination::NonNullEdgeTransfer> {
  size_t operator()(const elimination::NonNullEdgeTransfer &Transfer) const {
    auto H = hash<llvm::Instruction *>{}(Transfer.Src);
    const auto Dst = hash<llvm::Instruction *>{}(Transfer.Dst);
    return H ^ (Dst + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2));
  }
};
} // namespace std

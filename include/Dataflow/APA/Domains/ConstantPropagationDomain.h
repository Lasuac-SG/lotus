#pragma once

#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"

#include "Dataflow/APA/Domains/CopyOnWriteMap.h"

#include <cstdint>

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

class ConstantPropagationValue {
public:
  enum class Kind : std::uint8_t { Unknown, Undef, Constant, Overdefined };

  ConstantPropagationValue() = default;

  static ConstantPropagationValue get(llvm::Constant *C) {
    if (llvm::isa_and_nonnull<llvm::UndefValue>(C))
      return getUndef();
    ConstantPropagationValue Result;
    Result.K = Kind::Constant;
    Result.C = C;
    return Result;
  }

  static ConstantPropagationValue getUndef() {
    ConstantPropagationValue Result;
    Result.K = Kind::Undef;
    return Result;
  }

  static ConstantPropagationValue getOverdefined() {
    ConstantPropagationValue Result;
    Result.K = Kind::Overdefined;
    return Result;
  }

  bool isUnknown() const { return K == Kind::Unknown; }
  bool isUndef() const { return K == Kind::Undef; }
  bool isConstant() const { return K == Kind::Constant; }
  bool isOverdefined() const { return K == Kind::Overdefined; }
  bool isNotConstant() const { return false; }
  llvm::Constant *getConstant() const { return isConstant() ? C : nullptr; }
  llvm::Constant *getNotConstant() const { return nullptr; }

  const llvm::ConstantInt *asConstantInteger() const {
    return llvm::dyn_cast_or_null<llvm::ConstantInt>(getConstant());
  }

  llvm::Constant *getCompare(llvm::CmpInst::Predicate Pred, llvm::Type *,
                             const ConstantPropagationValue &Other) const {
    if (!isConstant() || !Other.isConstant())
      return nullptr;
    return llvm::ConstantExpr::getCompare(Pred, getConstant(),
                                          Other.getConstant());
  }

  bool mergeIn(const ConstantPropagationValue &Other) {
    if (Other.isUnknown() || isOverdefined())
      return false;
    if (isUnknown()) {
      *this = Other;
      return true;
    }
    if (K == Other.K && C == Other.C)
      return false;
    *this = getOverdefined();
    return true;
  }

  friend bool operator==(const ConstantPropagationValue &Lhs,
                         const ConstantPropagationValue &Rhs) {
    return Lhs.K == Rhs.K && Lhs.C == Rhs.C;
  }

private:
  Kind K = Kind::Unknown;
  llvm::Constant *C = nullptr;
};
using ConstantPropagationMap =
    CopyOnWriteMap<const llvm::Value *, ConstantPropagationValue>;

struct ConstantPropagationDomain {
  using value_type = ConstantPropagationMap;
  ConstantPropagationDomain() : U(value_type::makeUniverse()) {}
  value_type bottom() const { return value_type(U); }

  value_type join(const value_type &Lhs, const value_type &Rhs) const {
    value_type Out = Lhs;
    for (const auto &Entry : Rhs) {
      auto It = Out.find(Entry.first);
      if (It == Out.end()) {
        Out.insert({Entry.first, Entry.second});
      } else {
        Out.set(Entry.first, joinValue(It->second, Entry.second));
      }
    }
    return Out;
  }

  bool equal(const value_type &Lhs, const value_type &Rhs) const {
    for (const auto &Entry : Lhs) {
      auto It = Rhs.find(Entry.first);
      if (It == Rhs.end()) {
        if (!Entry.second.isUnknown())
          return false;
        continue;
      }
      if (!valueEqual(Entry.second, It->second))
        return false;
    }
    for (const auto &Entry : Rhs) {
      auto It = Lhs.find(Entry.first);
      if (It == Lhs.end() && !Entry.second.isUnknown())
        return false;
    }
    return true;
  }

private:
  typename value_type::universe_ptr U;

  static ConstantPropagationValue
  joinValue(const ConstantPropagationValue &Lhs,
            const ConstantPropagationValue &Rhs) {
    if (valueEqual(Lhs, Rhs) || Rhs.isUnknown())
      return Lhs;
    if (Lhs.isUnknown())
      return Rhs;

    return ConstantPropagationValue::getOverdefined();
  }

  static bool valueEqual(const ConstantPropagationValue &Lhs,
                         const ConstantPropagationValue &Rhs) {
    if (Lhs.isUnknown() || Rhs.isUnknown())
      return Lhs.isUnknown() && Rhs.isUnknown();
    if (Lhs.isUndef() || Rhs.isUndef())
      return Lhs.isUndef() && Rhs.isUndef();
    if (Lhs.isOverdefined() || Rhs.isOverdefined())
      return Lhs.isOverdefined() && Rhs.isOverdefined();
    if (Lhs.isConstant() || Rhs.isConstant())
      return Lhs.isConstant() && Rhs.isConstant() &&
             Lhs.getConstant() == Rhs.getConstant();
    return false;
  }
};

} // namespace elimination

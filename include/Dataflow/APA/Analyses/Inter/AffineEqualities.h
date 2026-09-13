#pragma once

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Domains/AffineRelationDomain.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace llvm {
class BasicBlock;
class Function;
class Value;
class Module;
} // namespace llvm

namespace elimination {

struct AffineFunctionKey {
  const llvm::Function *function = nullptr;

  bool operator<(const AffineFunctionKey &other) const {
    return function < other.function;
  }
};

struct AffineBlockKey {
  const llvm::BasicBlock *block = nullptr;

  bool operator<(const AffineBlockKey &other) const {
    return block < other.block;
  }
};

struct AffineExpr {
  bool top = true;
  int64_t constant = 0;
  std::unordered_map<const llvm::Value *, int64_t> terms;

  bool operator==(const AffineExpr &other) const {
    return top == other.top && constant == other.constant &&
           terms == other.terms;
  }
};

struct AffineEquality {
  unsigned bitWidth = 0;
  int64_t constant = 0;
  std::unordered_map<const llvm::Value *, int64_t> terms;

  bool operator==(const AffineEquality &other) const {
    return bitWidth == other.bitWidth && constant == other.constant &&
           terms == other.terms;
  }
};

struct AffineState {
  bool reachable = false;
  std::unordered_map<const llvm::Value *, AffineExpr> values;
  std::vector<AffineEquality> equalities;

  bool operator==(const AffineState &other) const {
    return reachable == other.reachable && values == other.values &&
           equalities == other.equalities;
  }
};

enum class InterAffineVocabularyMode { AllScalars, ObservableSlice };

struct InterAffineEqualitiesOptions {
  explicit InterAffineEqualitiesOptions(
      InterAffineVocabularyMode Vocabulary =
          InterAffineVocabularyMode::AllScalars,
      bool Verbose = false, std::size_t MaxTrackedValues = 0)
      : vocabulary(Vocabulary), verbose(Verbose),
        maxTrackedValues(MaxTrackedValues) {}

  InterAffineVocabularyMode vocabulary;
  bool verbose;
  // Zero means unlimited. Values outside a bounded observable slice are
  // soundly treated as untracked/havoced.
  std::size_t maxTrackedValues;
};

struct InterAffineEqualitiesResult {
  SolveStatus status = SolveStatus::Ok;
  std::size_t trackedValues = 0;
  std::map<AffineFunctionKey, AffineRelationDomain::value_type> summaries;
  std::map<AffineBlockKey, AffineRelationDomain::value_type> blockRelations;
};

InterAffineEqualitiesResult
runInterElimAffineEqualities(llvm::Module &M,
                             InterAffineEqualitiesOptions options =
                                 InterAffineEqualitiesOptions());

AffineState
materializeAffineExpressions(const AffineRelationDomain::value_type &relation);

} // namespace elimination

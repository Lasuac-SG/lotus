#pragma once

#include "CFL/InterleavedDyck/AffineSPDS/Matrix.h"

#include <memory>
#include <optional>

#include <llvm/ADT/SmallVector.h>

namespace lotus::cfl::interleaved_dyck::affine {

struct AffineDelta {
  llvm::SmallVector<std::size_t, 1> pivots;
  bool full = false;
  bool empty() const { return !full && pivots.empty(); }
  void clear() {
    pivots.clear();
    full = false;
  }
};

// Canonical reduced row-echelon basis over GF(2), with increasing pivots.
class LinearBasis {
public:
  explicit LinearBasis(std::size_t coordinates) : coordinates_(coordinates) {}
  bool insert(BitVector vector, AlgebraStatistics *statistics = nullptr);
  bool insertWithPivot(BitVector vector, std::size_t &new_pivot,
                       AlgebraStatistics *statistics = nullptr);
  bool insertBatch(llvm::SmallVectorImpl<BitVector> &vectors);
  void reduceInPlace(BitVector &vector,
                     AlgebraStatistics *statistics = nullptr) const;
  BitVector reduce(BitVector vector,
                   AlgebraStatistics *statistics = nullptr) const;
  const llvm::SmallVectorImpl<BitVector> &rows() const;
  const llvm::SmallVectorImpl<std::size_t> &pivots() const;
  std::size_t rank() const;
  bool operator==(const LinearBasis &other) const;

private:
  struct Storage {
    llvm::SmallVector<BitVector, 2> rows;
    llvm::SmallVector<std::size_t, 2> pivots;
  };
  friend class AffineSpace;
  bool insertEchelon(BitVector vector, AlgebraStatistics *statistics = nullptr);
  bool insertEchelonWithPivot(BitVector vector, std::size_t &new_pivot,
                              AlgebraStatistics *statistics = nullptr);
  void canonicalize(AlgebraStatistics *statistics = nullptr);
  Storage &write(AlgebraStatistics *statistics = nullptr);
  friend struct SeparationCertificate;
  friend std::optional<struct SeparationCertificate>
  separate(const class AffineSpace &, const class AffineSpace &,
           AlgebraStatistics *);
  std::size_t coordinates_;
  std::shared_ptr<Storage> storage_;
};

// Empty or a + span(B). The representative a is reduced by B, hence equality
// is semantic and independent of insertion order. A singleton ZERO MATRIX is
// nonempty and must never be confused with the empty weight.
class AffineSpace {
public:
  explicit AffineSpace(
      std::size_t dimension = 1,
      std::shared_ptr<const MatrixLayout> layout = nullptr); // empty
  static AffineSpace
  singleton(const Matrix &point,
            std::shared_ptr<const MatrixLayout> layout = nullptr);
  static AffineSpace top(std::size_t dimension);
  std::size_t dimension() const { return dimension_; }
  bool empty() const { return empty_; }
  std::size_t rank() const { return basis_.rank(); }
  std::size_t coordinates() const { return offset_.size(); }
  // Offset/directions use this space's coordinate layout. Decode to obtain a
  // public dense matrix; compressed coordinates still share ONE joint basis.
  Matrix decodeEntries(const BitVector &entries) const;
  const BitVector &offset() const { return offset_; }
  const llvm::SmallVectorImpl<BitVector> &directions() const {
    return basis_.rows();
  }
  Matrix representative() const;
  bool addPoint(const Matrix &point);
  bool joinWith(const AffineSpace &other,
                AlgebraStatistics *statistics = nullptr);
  bool joinWithDelta(const AffineSpace &other, AffineDelta *delta,
                     AlgebraStatistics *statistics = nullptr);
  bool contains(const Matrix &point) const;
  bool contains(const AffineSpace &other) const;
  bool intersects(const AffineSpace &other,
                  AlgebraStatistics *statistics = nullptr) const;
  bool isIdentity() const;
  AffineSpace product(const AffineSpace &right,
                      AlgebraStatistics *statistics = nullptr) const;
  bool joinProduct(const AffineSpace &left, const AffineSpace &right);
  AffineSpace block(std::size_t offset, std::size_t dimension) const;
  bool operator==(const AffineSpace &other) const;
  bool operator!=(const AffineSpace &other) const { return !(*this == other); }

private:
  friend class AffineSemiring;
  friend struct SeparationCertificate;
  friend std::optional<SeparationCertificate>
  separate(const AffineSpace &, const AffineSpace &, AlgebraStatistics *);
  bool compatibleLayout(const AffineSpace &other) const;
  AffineSpace expanded() const;
  bool identityPoint(const BitVector &point) const;
  bool joinProduct(const AffineSpace &left, const AffineSpace &right,
                   const RightMatrixMultiplier *prepared_right,
                   AlgebraStatistics *statistics = nullptr);
  bool joinProductWithDelta(const AffineSpace &left, const AffineSpace &right,
                            const RightMatrixMultiplier *prepared_right,
                            AffineDelta *delta,
                            AlgebraStatistics *statistics = nullptr);
  bool joinProductDelta(const AffineSpace &left, const AffineSpace &right,
                        const AffineDelta &input, bool input_is_left,
                        AffineDelta *output,
                        const RightMatrixMultiplier *prepared_right = nullptr,
                        AlgebraStatistics *statistics = nullptr);
  bool joinTripleProductDelta(const AffineSpace &left,
                              const AffineSpace &middle,
                              const AffineSpace &right,
                              const AffineDelta &input, bool input_is_middle,
                              AffineDelta *output,
                              AlgebraStatistics *statistics = nullptr);
  bool addDirection(BitVector direction);
  void check(std::size_t dimension) const;
  std::size_t dimension_;
  std::shared_ptr<const MatrixLayout> layout_;
  bool empty_ = true;
  bool is_identity_ = false;
  BitVector offset_;
  LinearBasis basis_;
};

// For two nonempty disjoint hulls: <functional,X> = left_value on the first
// hull and right_value on the second, with distinct values. verify() checks
// the algebraic certificate against the supplied hulls, NOT their provenance.
struct SeparationCertificate {
  Matrix functional;
  bool left_value = false;
  bool right_value = true;
  bool verify(const AffineSpace &left, const AffineSpace &right) const;
};
std::optional<SeparationCertificate>
separate(const AffineSpace &left, const AffineSpace &right,
         AlgebraStatistics *statistics = nullptr);

// Finite-height idempotent semiring for the existing SPDS saturation engine.
// combine = affine hull of union (NOT XOR); extend = affine hull of products.
class AffineSemiring {
public:
  using Weight = AffineSpace;
  using Delta = AffineDelta;
  using PreparedWeight = std::shared_ptr<const RightMatrixMultiplier>;
  explicit AffineSemiring(std::size_t dimension = 1,
                          std::shared_ptr<const MatrixLayout> layout = nullptr);
  std::size_t dimension() const { return dimension_; }
  std::size_t coordinates() const { return zero_.coordinates(); }
  const std::shared_ptr<const MatrixLayout> &layout() const { return layout_; }
  const std::shared_ptr<AlgebraStatistics> &statistics() const {
    return statistics_;
  }
  AffineSemiring
  withStatistics(std::shared_ptr<AlgebraStatistics> statistics) const {
    auto copy = *this;
    copy.statistics_ = std::move(statistics);
    return copy;
  }
  const Weight &zero() const { return zero_; }
  const Weight &one() const { return one_; }
  Weight lift(const Matrix &matrix) const;
  Weight combine(const Weight &left, const Weight &right) const;
  bool combineWith(Weight &left, const Weight &right) const;
  bool combineWithDelta(Weight &left, const Weight &right, Delta *delta) const;
  Weight extend(const Weight &left, const Weight &right) const;
  bool extendAndCombine(Weight &target, const Weight &left,
                        const Weight &right) const;
  bool extendAndCombineDelta(Weight &target, const Weight &left,
                             const Weight &right, Delta *delta) const;
  bool extendDeltaAndCombine(Weight &target, const Weight &left,
                             const Weight &right, const Delta &input,
                             bool input_is_left, Delta *output) const;
  PreparedWeight prepareWeight(const Weight &weight) const;
  Weight extendPrepared(const Weight &left, const Weight &right,
                        const PreparedWeight &prepared_right) const;
  bool extendAndCombinePrepared(Weight &target, const Weight &left,
                                const Weight &right,
                                const PreparedWeight &prepared_right) const;
  bool extendAndCombinePreparedDelta(Weight &target, const Weight &left,
                                     const Weight &right,
                                     const PreparedWeight &prepared_right,
                                     Delta *delta) const;
  bool extendPreparedInputDeltaAndCombine(Weight &target, const Weight &left,
                                          const Weight &right,
                                          const PreparedWeight &prepared_right,
                                          const Delta &input,
                                          Delta *output) const;
  bool extendPushDeltaAndCombine(Weight &target, const Weight &rule,
                                 const Weight &first, const Weight &second,
                                 const Delta &input, bool input_is_first,
                                 Delta *output) const;
  void mergeDelta(Delta &target, Delta source) const;

private:
  void check(const Weight &weight) const;
  std::size_t dimension_;
  std::shared_ptr<const MatrixLayout> layout_;
  std::shared_ptr<AlgebraStatistics> statistics_;
  Matrix identity_;
  Weight zero_;
  Weight one_;
};

} // namespace lotus::cfl::interleaved_dyck::affine

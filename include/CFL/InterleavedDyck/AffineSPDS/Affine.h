#pragma once

#include "CFL/InterleavedDyck/AffineSPDS/Matrix.h"

#include <optional>

#include <llvm/ADT/SmallVector.h>

namespace lotus::cfl::interleaved_dyck::affine {

// Canonical reduced row-echelon basis over GF(2), with increasing pivots.
class LinearBasis {
public:
  explicit LinearBasis(std::size_t coordinates) : coordinates_(coordinates) {}
  bool insert(BitVector vector);
  BitVector reduce(BitVector vector) const;
  const llvm::SmallVectorImpl<BitVector> &rows() const { return rows_; }
  std::size_t rank() const { return rows_.size(); }

private:
  std::size_t coordinates_;
  llvm::SmallVector<BitVector, 2> rows_;
};

// Empty or a + span(B). The representative a is reduced by B, hence equality
// is semantic and independent of insertion order. A singleton ZERO MATRIX is
// nonempty and must never be confused with the empty weight.
class AffineSpace {
public:
  explicit AffineSpace(std::size_t dimension = 1); // empty
  static AffineSpace singleton(const Matrix &point);
  static AffineSpace top(std::size_t dimension);
  std::size_t dimension() const { return dimension_; }
  bool empty() const { return empty_; }
  std::size_t rank() const { return basis_.rank(); }
  const BitVector &offset() const { return offset_; }
  const llvm::SmallVectorImpl<BitVector> &directions() const {
    return basis_.rows();
  }
  Matrix representative() const;
  bool addPoint(const Matrix &point);
  bool joinWith(const AffineSpace &other);
  bool contains(const Matrix &point) const;
  bool contains(const AffineSpace &other) const;
  bool intersects(const AffineSpace &other) const;
  bool isIdentity() const;
  AffineSpace product(const AffineSpace &right) const;
  AffineSpace block(std::size_t offset, std::size_t dimension) const;
  bool operator==(const AffineSpace &other) const;
  bool operator!=(const AffineSpace &other) const { return !(*this == other); }

private:
  bool addDirection(BitVector direction);
  void check(std::size_t dimension) const;
  std::size_t dimension_;
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
std::optional<SeparationCertificate> separate(const AffineSpace &left,
                                              const AffineSpace &right);

// Finite-height idempotent semiring for the existing SPDS saturation engine.
// combine = affine hull of union (NOT XOR); extend = affine hull of products.
class AffineSemiring {
public:
  using Weight = AffineSpace;
  explicit AffineSemiring(std::size_t dimension = 1);
  std::size_t dimension() const { return dimension_; }
  const Weight &zero() const { return zero_; }
  const Weight &one() const { return one_; }
  Weight lift(const Matrix &matrix) const;
  Weight combine(const Weight &left, const Weight &right) const;
  bool combineWith(Weight &left, const Weight &right) const;
  Weight extend(const Weight &left, const Weight &right) const;

private:
  void check(const Weight &weight) const;
  std::size_t dimension_;
  Matrix identity_;
  Weight zero_;
  Weight one_;
};

} // namespace lotus::cfl::interleaved_dyck::affine

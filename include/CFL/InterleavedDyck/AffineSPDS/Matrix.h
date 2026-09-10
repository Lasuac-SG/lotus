#pragma once

#include "CFL/InterleavedDyck/AffineSPDS/Statistics.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::affine {

class RightMatrixMultiplier;

// Packed vectors over GF(2). Logical lengths, including non-word-aligned tails,
// are checked. No 32/64-dimensional limit and no external algebra dependency.
class BitVector {
public:
  explicit BitVector(std::size_t size = 0);
  BitVector(const BitVector &other);
  BitVector &operator=(const BitVector &other);
  BitVector(BitVector &&) noexcept = default;
  BitVector &operator=(BitVector &&) noexcept = default;
  std::size_t size() const { return size_; }
  bool test(std::size_t bit) const;
  void set(std::size_t bit, bool value = true);
  bool empty() const;           // all coordinates zero
  std::size_t firstSet() const; // size() if zero
  BitVector &operator^=(const BitVector &other);
  BitVector operator^(const BitVector &other) const;
  bool dot(const BitVector &other) const;
  bool operator==(const BitVector &other) const;
  bool operator!=(const BitVector &other) const { return !(*this == other); }

private:
  friend class AffineSpace;
  friend class LinearBasis;
  friend class Matrix;
  friend class RightMatrixMultiplier;
  static constexpr std::size_t INLINE_WORDS = 4;
  std::size_t wordCount() const { return word_count_; }
  std::uint64_t *data() {
    return word_count_ <= INLINE_WORDS ? inline_words_.data()
                                       : heap_words_.get();
  }
  const std::uint64_t *data() const {
    return word_count_ <= INLINE_WORDS ? inline_words_.data()
                                       : heap_words_.get();
  }
  std::uint64_t &word(std::size_t index) { return data()[index]; }
  const std::uint64_t &word(std::size_t index) const { return data()[index]; }
  bool isIdentityMatrix(std::size_t dimension) const;
  std::uint64_t extract(std::size_t offset, std::size_t count) const;
  void xorChunk(std::size_t offset, std::size_t count, std::uint64_t value);
  std::size_t size_;
  std::size_t word_count_;
  std::array<std::uint64_t, INLINE_WORDS> inline_words_{};
  std::unique_ptr<std::uint64_t[]> heap_words_;
};

class Matrix {
public:
  explicit Matrix(
      std::size_t dimension = 1); // the zero MATRIX, not semiring zero
  Matrix(std::size_t dimension, BitVector entries);
  static Matrix identity(std::size_t dimension);
  static Matrix parse(const std::string &rows); // e.g. "110/011/001"
  static Matrix directSum(const std::vector<Matrix> &blocks);
  static std::size_t coordinateCount(std::size_t dimension);
  std::size_t dimension() const { return dimension_; }
  std::size_t coordinates() const { return entries_.size(); }
  const BitVector &entries() const & { return entries_; }
  BitVector entries() && { return std::move(entries_); }
  bool get(std::size_t row, std::size_t column) const;
  void set(std::size_t row, std::size_t column, bool value = true);
  bool isZero() const { return entries_.empty(); }
  bool isIdentity() const;
  Matrix operator*(const Matrix &right) const;
  Matrix operator^(const Matrix &right) const;
  bool dot(const Matrix &right) const { return entries_.dot(right.entries_); }
  Matrix block(std::size_t offset, std::size_t dimension) const;
  std::string str() const;
  bool operator==(const Matrix &right) const;
  bool operator!=(const Matrix &right) const { return !(*this == right); }

private:
  friend class RightMatrixMultiplier;
  std::size_t dimension_;
  BitVector entries_;
};

// Exact embedding of block-diagonal matrices into concatenated block entries.
// This is a coordinate layout, not a product of independent affine analyses.
class MatrixLayout {
public:
  struct Block {
    std::size_t offset, dimension, coordinate;
  };
  MatrixLayout(std::size_t dimension,
               const std::vector<std::pair<std::size_t, std::size_t>> &blocks);
  std::size_t dimension() const { return dimension_; }
  std::size_t coordinates() const { return identity_.size(); }
  const std::vector<Block> &blocks() const { return blocks_; }
  const BitVector &identity() const { return identity_; }
  bool contains(const Matrix &matrix) const;
  BitVector pack(const Matrix &matrix) const;
  BitVector project(const Matrix &matrix) const;
  Matrix unpack(const BitVector &entries) const;
  std::size_t coordinate(std::size_t row, std::size_t column) const;
  bool operator==(const MatrixLayout &other) const;

private:
  std::size_t dimension_;
  std::vector<Block> blocks_;
  std::vector<std::size_t> row_blocks_;
  BitVector identity_;
};

// Reuses unpacked rows across products; the right matrix must outlive this.
class RightMatrixMultiplier {
public:
  explicit RightMatrixMultiplier(const Matrix &right);
  RightMatrixMultiplier(std::size_t dimension, const BitVector &right);
  RightMatrixMultiplier(std::size_t dimension, const BitVector &right,
                        std::shared_ptr<const MatrixLayout> layout);
  BitVector multiply(const BitVector &left,
                     AlgebraStatistics *statistics = nullptr) const;
  Matrix multiply(const Matrix &left) const;

private:
  friend class AffineSpace;
  static const std::uint64_t *checkedWords(std::size_t dimension,
                                           const BitVector &vector);
  RightMatrixMultiplier(std::size_t dimension, const std::uint64_t *right);
  BitVector multiply(const std::uint64_t *left,
                     AlgebraStatistics *statistics = nullptr) const;
  std::size_t dimension_;
  const std::uint64_t *right_;
  std::array<std::uint64_t, 64> rows_{};
  std::shared_ptr<const MatrixLayout> layout_;
};

} // namespace lotus::cfl::interleaved_dyck::affine

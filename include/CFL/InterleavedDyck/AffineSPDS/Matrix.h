#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lotus::cfl::interleaved_dyck::affine {

// Packed vectors over GF(2). Logical lengths, including non-word-aligned tails,
// are checked. No 32/64-dimensional limit and no external algebra dependency.
class BitVector {
public:
  explicit BitVector(std::size_t size = 0);
  std::size_t size() const { return size_; }
  bool test(std::size_t bit) const;
  void set(std::size_t bit, bool value = true);
  bool empty() const; // all coordinates zero
  std::size_t firstSet() const; // size() if zero
  BitVector &operator^=(const BitVector &other);
  BitVector operator^(const BitVector &other) const;
  bool dot(const BitVector &other) const;
  bool operator==(const BitVector &other) const;
  bool operator!=(const BitVector &other) const { return !(*this == other); }
private:
  friend class Matrix;
  std::uint64_t extract(std::size_t offset, std::size_t count) const;
  void xorChunk(std::size_t offset, std::size_t count, std::uint64_t value);
  std::size_t size_;
  std::vector<std::uint64_t> words_;
};

class Matrix {
public:
  explicit Matrix(std::size_t dimension = 1); // the zero MATRIX, not semiring zero
  Matrix(std::size_t dimension, BitVector entries);
  static Matrix identity(std::size_t dimension);
  static Matrix parse(const std::string &rows); // e.g. "110/011/001"
  static Matrix directSum(const std::vector<Matrix> &blocks);
  std::size_t dimension() const { return dimension_; }
  std::size_t coordinates() const { return entries_.size(); }
  const BitVector &entries() const { return entries_; }
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
  std::size_t dimension_;
  BitVector entries_;
};

} // namespace lotus::cfl::interleaved_dyck::affine

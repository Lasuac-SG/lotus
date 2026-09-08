#include "CFL/InterleavedDyck/AffineSPDS/Matrix.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lotus::cfl::interleaved_dyck::affine {
namespace {
std::size_t square(std::size_t n) {
  if (!n || n > std::numeric_limits<std::size_t>::max() / n)
    throw std::invalid_argument("matrix dimension must be positive and representable");
  return n * n;
}
unsigned trailing(std::uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<unsigned>(__builtin_ctzll(x));
#else
  unsigned n = 0;
  while ((x & 1U) == 0) { ++n; x >>= 1U; }
  return n;
#endif
}
bool parity(std::uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_parityll(x) != 0;
#else
  bool p = false;
  while (x) { p = !p; x &= x - 1; }
  return p;
#endif
}
void compatible(std::size_t a, std::size_t b) {
  if (a != b) throw std::invalid_argument("incompatible GF(2) dimensions");
}
}
BitVector::BitVector(std::size_t size)
    : size_(size), words_(size / 64 + (size % 64 != 0), 0) {}
bool BitVector::test(std::size_t bit) const {
  if (bit >= size_) throw std::out_of_range("GF(2) coordinate");
  return (words_[bit / 64] >> (bit % 64)) & 1U;
}
void BitVector::set(std::size_t bit, bool value) {
  if (bit >= size_) throw std::out_of_range("GF(2) coordinate");
  const auto mask = std::uint64_t{1} << (bit % 64);
  if (value) words_[bit / 64] |= mask;
  else words_[bit / 64] &= ~mask;
}
bool BitVector::empty() const {
  return std::all_of(words_.begin(), words_.end(), [](auto x) { return x == 0; });
}
std::size_t BitVector::firstSet() const {
  for (std::size_t i = 0; i < words_.size(); ++i)
    if (words_[i]) return i * 64 + trailing(words_[i]);
  return size_;
}
BitVector &BitVector::operator^=(const BitVector &other) {
  compatible(size_, other.size_);
  for (std::size_t i = 0; i < words_.size(); ++i) words_[i] ^= other.words_[i];
  return *this;
}
BitVector BitVector::operator^(const BitVector &other) const {
  BitVector result = *this; result ^= other; return result;
}
bool BitVector::dot(const BitVector &other) const {
  compatible(size_, other.size_); bool result = false;
  for (std::size_t i = 0; i < words_.size(); ++i)
    result ^= parity(words_[i] & other.words_[i]);
  return result;
}
bool BitVector::operator==(const BitVector &other) const {
  return size_ == other.size_ && words_ == other.words_;
}
std::uint64_t BitVector::extract(std::size_t offset, std::size_t count) const {
  // Internal callers guarantee 1 <= count <= 64 and offset+count <= size_.
  const auto shift = offset % 64;
  std::uint64_t value = words_[offset / 64] >> shift;
  if (shift && count > 64 - shift)
    value |= words_[offset / 64 + 1] << (64 - shift);
  if (count < 64) value &= (std::uint64_t{1} << count) - 1;
  return value;
}
void BitVector::xorChunk(std::size_t offset, std::size_t count, std::uint64_t value) {
  const auto shift = offset % 64;
  words_[offset / 64] ^= value << shift;
  if (shift && count > 64 - shift) words_[offset / 64 + 1] ^= value >> (64 - shift);
}
Matrix::Matrix(std::size_t dimension) : dimension_(dimension), entries_(square(dimension)) {}
Matrix::Matrix(std::size_t dimension, BitVector entries)
    : dimension_(dimension), entries_(std::move(entries)) {
  compatible(square(dimension), entries_.size());
}
Matrix Matrix::identity(std::size_t dimension) {
  Matrix m(dimension);
  for (std::size_t i = 0; i < dimension; ++i) m.set(i, i);
  return m;
}
bool Matrix::get(std::size_t row, std::size_t column) const {
  if (row >= dimension_ || column >= dimension_) throw std::out_of_range("matrix index");
  return entries_.test(row * dimension_ + column);
}
void Matrix::set(std::size_t row, std::size_t column, bool value) {
  if (row >= dimension_ || column >= dimension_) throw std::out_of_range("matrix index");
  entries_.set(row * dimension_ + column, value);
}
bool Matrix::isIdentity() const { return *this == identity(dimension_); }
Matrix Matrix::operator*(const Matrix &right) const {
  compatible(dimension_, right.dimension_);
  Matrix result(dimension_);
  // XOR packed row slices instead of multiplying individual scalar entries.
  for (std::size_t i = 0; i < dimension_; ++i)
    for (std::size_t k = 0; k < dimension_; ++k)
      if (get(i, k))
        for (std::size_t j = 0; j < dimension_; j += 64) {
          const auto count = std::min<std::size_t>(64, dimension_ - j);
          result.entries_.xorChunk(i * dimension_ + j, count,
              right.entries_.extract(k * dimension_ + j, count));
        }
  return result;
}
Matrix Matrix::operator^(const Matrix &right) const {
  compatible(dimension_, right.dimension_);
  return Matrix(dimension_, entries_ ^ right.entries_);
}
bool Matrix::operator==(const Matrix &right) const {
  return dimension_ == right.dimension_ && entries_ == right.entries_;
}
Matrix Matrix::block(std::size_t offset, std::size_t dimension) const {
  if (!dimension || offset > dimension_ || dimension > dimension_ - offset)
    throw std::out_of_range("matrix block");
  Matrix result(dimension);
  for (std::size_t i = 0; i < dimension; ++i)
    for (std::size_t j = 0; j < dimension; ++j) result.set(i, j, get(offset+i, offset+j));
  return result;
}
Matrix Matrix::directSum(const std::vector<Matrix> &blocks) {
  if (blocks.empty()) return identity(1);
  std::size_t size = 0;
  for (const auto &m : blocks) {
    if (m.dimension() > std::numeric_limits<std::size_t>::max() - size)
      throw std::length_error("direct-sum dimension overflow");
    size += m.dimension();
  }
  Matrix result(size); std::size_t offset = 0;
  for (const auto &m : blocks) {
    for (std::size_t i = 0; i < m.dimension(); ++i)
      for (std::size_t j = 0; j < m.dimension(); ++j)
        if (m.get(i, j)) result.set(offset+i, offset+j);
    offset += m.dimension();
  }
  return result;
}
std::string Matrix::str() const {
  std::string result;
  for (std::size_t i = 0; i < dimension_; ++i) {
    if (i) result += '/';
    for (std::size_t j = 0; j < dimension_; ++j) result += get(i, j) ? '1' : '0';
  }
  return result;
}
Matrix Matrix::parse(const std::string &rows) {
  const auto n = static_cast<std::size_t>(std::count(rows.begin(), rows.end(), '/')) + 1;
  if (square(n) > std::numeric_limits<std::size_t>::max() - (n - 1) ||
      rows.size() != n * n + n - 1)
    throw std::invalid_argument("matrix must contain square slash-separated bit rows");
  Matrix result(n); std::size_t at = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (i && rows[at++] != '/') throw std::invalid_argument("invalid matrix row boundary");
    for (std::size_t j = 0; j < n; ++j) {
      const char c = rows[at++];
      if (c != '0' && c != '1') throw std::invalid_argument("non-binary matrix entry");
      result.set(i, j, c == '1');
    }
  }
  return result;
}
} // namespace lotus::cfl::interleaved_dyck::affine

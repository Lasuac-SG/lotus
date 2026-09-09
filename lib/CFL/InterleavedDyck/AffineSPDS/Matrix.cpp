#include "CFL/InterleavedDyck/AffineSPDS/Matrix.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lotus::cfl::interleaved_dyck::affine {
namespace {
std::size_t square(std::size_t n) {
  if (!n || n > std::numeric_limits<std::size_t>::max() / n)
    throw std::invalid_argument(
        "matrix dimension must be positive and representable");
  return n * n;
}
unsigned trailing(std::uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<unsigned>(__builtin_ctzll(x));
#else
  unsigned n = 0;
  while ((x & 1U) == 0) {
    ++n;
    x >>= 1U;
  }
  return n;
#endif
}
bool parity(std::uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_parityll(x) != 0;
#else
  bool p = false;
  while (x) {
    p = !p;
    x &= x - 1;
  }
  return p;
#endif
}
void compatible(std::size_t a, std::size_t b) {
  if (a != b)
    throw std::invalid_argument("incompatible GF(2) dimensions");
}
std::uint64_t extractWords(const std::uint64_t *words, std::size_t offset,
                           std::size_t count) {
  const auto shift = offset % 64;
  std::uint64_t value = words[offset / 64] >> shift;
  if (shift && count > 64 - shift)
    value |= words[offset / 64 + 1] << (64 - shift);
  if (count < 64)
    value &= (std::uint64_t{1} << count) - 1;
  return value;
}
} // namespace
BitVector::BitVector(std::size_t size)
    : size_(size), word_count_(size / 64 + (size % 64 != 0)) {
  if (wordCount() > INLINE_WORDS)
    heap_words_ = std::make_unique<std::uint64_t[]>(wordCount());
}
BitVector::BitVector(const BitVector &other)
    : size_(other.size_), word_count_(other.word_count_),
      inline_words_(other.inline_words_) {
  if (wordCount() > INLINE_WORDS) {
    heap_words_ = std::make_unique<std::uint64_t[]>(wordCount());
    std::copy_n(other.data(), wordCount(), data());
  }
}
BitVector &BitVector::operator=(const BitVector &other) {
  if (this == &other)
    return *this;
  const std::size_t old_word_count = word_count_;
  size_ = other.size_;
  word_count_ = other.word_count_;
  inline_words_ = other.inline_words_;
  if (wordCount() <= INLINE_WORDS) {
    heap_words_.reset();
  } else {
    if (old_word_count != wordCount() || !heap_words_)
      heap_words_ = std::make_unique<std::uint64_t[]>(wordCount());
    std::copy_n(other.data(), wordCount(), data());
  }
  return *this;
}
bool BitVector::test(std::size_t bit) const {
  if (bit >= size_)
    throw std::out_of_range("GF(2) coordinate");
  return (word(bit / 64) >> (bit % 64)) & 1U;
}
void BitVector::set(std::size_t bit, bool value) {
  if (bit >= size_)
    throw std::out_of_range("GF(2) coordinate");
  const auto mask = std::uint64_t{1} << (bit % 64);
  if (value)
    word(bit / 64) |= mask;
  else
    word(bit / 64) &= ~mask;
}
bool BitVector::empty() const {
  const auto *words = data();
  for (std::size_t i = 0; i < wordCount(); ++i)
    if (words[i] != 0)
      return false;
  return true;
}
std::size_t BitVector::firstSet() const {
  const auto *words = data();
  for (std::size_t i = 0; i < wordCount(); ++i)
    if (words[i])
      return i * 64 + trailing(words[i]);
  return size_;
}
BitVector &BitVector::operator^=(const BitVector &other) {
  compatible(size_, other.size_);
  auto *left = data();
  const auto *right = other.data();
  if (wordCount() <= INLINE_WORDS) {
    switch (wordCount()) {
    case 4:
      left[3] ^= right[3];
      [[fallthrough]];
    case 3:
      left[2] ^= right[2];
      [[fallthrough]];
    case 2:
      left[1] ^= right[1];
      [[fallthrough]];
    case 1:
      left[0] ^= right[0];
      [[fallthrough]];
    case 0:
      return *this;
    }
  }
  for (std::size_t i = 0; i < wordCount(); ++i)
    left[i] ^= right[i];
  return *this;
}
BitVector BitVector::operator^(const BitVector &other) const {
  BitVector result = *this;
  result ^= other;
  return result;
}
bool BitVector::dot(const BitVector &other) const {
  compatible(size_, other.size_);
  const auto *left = data();
  const auto *right = other.data();
  std::uint64_t products = 0;
  for (std::size_t i = 0; i < wordCount(); ++i)
    products ^= left[i] & right[i];
  return parity(products);
}
bool BitVector::operator==(const BitVector &other) const {
  if (size_ != other.size_)
    return false;
  const auto *left = data();
  const auto *right = other.data();
  for (std::size_t i = 0; i < wordCount(); ++i)
    if (left[i] != right[i])
      return false;
  return true;
}
bool BitVector::isIdentityMatrix(std::size_t dimension) const {
  if (dimension == 0 ||
      dimension > std::numeric_limits<std::size_t>::max() / dimension ||
      size_ != dimension * dimension)
    return false;
  const auto *words = data();
  std::size_t diagonal = 0;
  for (std::size_t w = 0; w < wordCount(); ++w) {
    std::uint64_t expected = 0;
    const std::size_t first = w * 64;
    const std::size_t last = std::min(size_, first + 64);
    while (diagonal < last) {
      expected |= std::uint64_t{1} << (diagonal - first);
      diagonal += dimension + 1;
    }
    if (words[w] != expected)
      return false;
  }
  return true;
}
std::uint64_t BitVector::extract(std::size_t offset, std::size_t count) const {
  // Internal callers guarantee 1 <= count <= 64 and offset+count <= size_.
  const auto *words = data();
  const auto shift = offset % 64;
  std::uint64_t value = words[offset / 64] >> shift;
  if (shift && count > 64 - shift)
    value |= words[offset / 64 + 1] << (64 - shift);
  if (count < 64)
    value &= (std::uint64_t{1} << count) - 1;
  return value;
}
void BitVector::xorChunk(std::size_t offset, std::size_t count,
                         std::uint64_t value) {
  auto *words = data();
  const auto shift = offset % 64;
  words[offset / 64] ^= value << shift;
  if (shift && count > 64 - shift)
    words[offset / 64 + 1] ^= value >> (64 - shift);
}
Matrix::Matrix(std::size_t dimension)
    : dimension_(dimension), entries_(square(dimension)) {}
Matrix::Matrix(std::size_t dimension, BitVector entries)
    : dimension_(dimension), entries_(std::move(entries)) {
  compatible(square(dimension), entries_.size());
}
Matrix Matrix::identity(std::size_t dimension) {
  Matrix m(dimension);
  for (std::size_t i = 0; i < dimension; ++i)
    m.set(i, i);
  return m;
}
bool Matrix::get(std::size_t row, std::size_t column) const {
  if (row >= dimension_ || column >= dimension_)
    throw std::out_of_range("matrix index");
  return entries_.test(row * dimension_ + column);
}
void Matrix::set(std::size_t row, std::size_t column, bool value) {
  if (row >= dimension_ || column >= dimension_)
    throw std::out_of_range("matrix index");
  entries_.set(row * dimension_ + column, value);
}
bool Matrix::isIdentity() const {
  return entries_.isIdentityMatrix(dimension_);
}
Matrix Matrix::operator*(const Matrix &right) const {
  compatible(dimension_, right.dimension_);
  return RightMatrixMultiplier(right).multiply(*this);
}
RightMatrixMultiplier::RightMatrixMultiplier(const Matrix &right)
    : RightMatrixMultiplier(right.dimension_, right.entries_) {}
const std::uint64_t *
RightMatrixMultiplier::checkedWords(std::size_t dimension,
                                    const BitVector &vector) {
  compatible(square(dimension), vector.size());
  return vector.data();
}
RightMatrixMultiplier::RightMatrixMultiplier(std::size_t dimension,
                                             const BitVector &right)
    : RightMatrixMultiplier(dimension, checkedWords(dimension, right)) {}
RightMatrixMultiplier::RightMatrixMultiplier(std::size_t dimension,
                                             const std::uint64_t *right)
    : dimension_(dimension), right_(dimension > 64 ? right : nullptr) {
  if (dimension_ > 64)
    return;
  const std::uint64_t row_mask = dimension_ == 64
                                     ? std::numeric_limits<std::uint64_t>::max()
                                     : (std::uint64_t{1} << dimension_) - 1;
  for (std::size_t row = 0; row < dimension_; ++row) {
    const std::size_t offset = row * dimension_;
    const std::size_t shift = offset % 64;
    std::uint64_t value = right[offset / 64] >> shift;
    if (shift && dimension_ > 64 - shift)
      value |= right[offset / 64 + 1] << (64 - shift);
    rows_[row] = value & row_mask;
  }
}
BitVector RightMatrixMultiplier::multiply(const BitVector &left) const {
  compatible(square(dimension_), left.size());
  return multiply(left.data());
}
BitVector RightMatrixMultiplier::multiply(const std::uint64_t *left) const {
  BitVector result(square(dimension_));
  if (dimension_ > 64) {
    for (std::size_t row = 0; row < dimension_; ++row)
      for (std::size_t block = 0; block < dimension_; block += 64) {
        const auto selector_count =
            std::min<std::size_t>(64, dimension_ - block);
        std::uint64_t selectors =
            extractWords(left, row * dimension_ + block, selector_count);
        while (selectors) {
          const std::size_t selected = block + trailing(selectors);
          selectors &= selectors - 1;
          for (std::size_t column = 0; column < dimension_; column += 64) {
            const auto count =
                std::min<std::size_t>(64, dimension_ - column);
            result.xorChunk(
                row * dimension_ + column, count,
                extractWords(right_, selected * dimension_ + column, count));
          }
        }
      }
    return result;
  }
  const auto *left_words = left;
  auto *result_words = result.data();
  const std::uint64_t row_mask = dimension_ == 64
                                     ? std::numeric_limits<std::uint64_t>::max()
                                     : (std::uint64_t{1} << dimension_) - 1;
  for (std::size_t row = 0; row < dimension_; ++row) {
    const std::size_t offset = row * dimension_;
    const std::size_t shift = offset % 64;
    std::uint64_t selectors = left_words[offset / 64] >> shift;
    if (shift && dimension_ > 64 - shift)
      selectors |= left_words[offset / 64 + 1] << (64 - shift);
    selectors &= row_mask;
    std::uint64_t product = 0;
    while (selectors) {
      product ^= rows_[trailing(selectors)];
      selectors &= selectors - 1;
    }
    result_words[offset / 64] ^= product << shift;
    if (shift && dimension_ > 64 - shift)
      result_words[offset / 64 + 1] ^= product >> (64 - shift);
  }
  return result;
}
Matrix RightMatrixMultiplier::multiply(const Matrix &left) const {
  compatible(left.dimension_, dimension_);
  return Matrix(dimension_, multiply(left.entries_));
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
    for (std::size_t j = 0; j < dimension; ++j)
      result.set(i, j, get(offset + i, offset + j));
  return result;
}
Matrix Matrix::directSum(const std::vector<Matrix> &blocks) {
  if (blocks.empty())
    return identity(1);
  std::size_t size = 0;
  for (const auto &m : blocks) {
    if (m.dimension() > std::numeric_limits<std::size_t>::max() - size)
      throw std::length_error("direct-sum dimension overflow");
    size += m.dimension();
  }
  Matrix result(size);
  std::size_t offset = 0;
  for (const auto &m : blocks) {
    for (std::size_t i = 0; i < m.dimension(); ++i)
      for (std::size_t j = 0; j < m.dimension(); ++j)
        if (m.get(i, j))
          result.set(offset + i, offset + j);
    offset += m.dimension();
  }
  return result;
}
std::string Matrix::str() const {
  std::string result;
  for (std::size_t i = 0; i < dimension_; ++i) {
    if (i)
      result += '/';
    for (std::size_t j = 0; j < dimension_; ++j)
      result += get(i, j) ? '1' : '0';
  }
  return result;
}
Matrix Matrix::parse(const std::string &rows) {
  const auto n =
      static_cast<std::size_t>(std::count(rows.begin(), rows.end(), '/')) + 1;
  if (square(n) > std::numeric_limits<std::size_t>::max() - (n - 1) ||
      rows.size() != n * n + n - 1)
    throw std::invalid_argument(
        "matrix must contain square slash-separated bit rows");
  Matrix result(n);
  std::size_t at = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (i && rows[at++] != '/')
      throw std::invalid_argument("invalid matrix row boundary");
    for (std::size_t j = 0; j < n; ++j) {
      const char c = rows[at++];
      if (c != '0' && c != '1')
        throw std::invalid_argument("non-binary matrix entry");
      result.set(i, j, c == '1');
    }
  }
  return result;
}
} // namespace lotus::cfl::interleaved_dyck::affine

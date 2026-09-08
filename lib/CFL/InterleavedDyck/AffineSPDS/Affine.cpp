#include "CFL/InterleavedDyck/AffineSPDS/Affine.h"
#include <algorithm>
#include <stdexcept>

namespace lotus::cfl::interleaved_dyck::affine {
BitVector LinearBasis::reduce(BitVector vector) const {
  if (vector.size() != coordinates_) throw std::invalid_argument("basis dimension mismatch");
  for (const auto &row : rows_)
    if (vector.test(row.firstSet())) vector ^= row;
  return vector;
}
bool LinearBasis::insert(BitVector vector) {
  vector = reduce(std::move(vector));
  const auto pivot = vector.firstSet();
  if (pivot == coordinates_) return false;
  for (auto &row : rows_) if (row.test(pivot)) row ^= vector;
  auto where = std::lower_bound(rows_.begin(), rows_.end(), pivot,
      [](const BitVector &row, std::size_t p) { return row.firstSet() < p; });
  rows_.insert(where, std::move(vector));
  return true;
}
AffineSpace::AffineSpace(std::size_t dimension)
    : dimension_(dimension), offset_(Matrix(dimension).coordinates()), basis_(offset_.size()) {}
AffineSpace AffineSpace::singleton(const Matrix &point) {
  AffineSpace result(point.dimension()); result.addPoint(point); return result;
}
AffineSpace AffineSpace::top(std::size_t dimension) {
  AffineSpace result = singleton(Matrix(dimension));
  for (std::size_t i = 0; i < result.offset_.size(); ++i) {
    BitVector unit(result.offset_.size()); unit.set(i); result.addDirection(std::move(unit));
  }
  return result;
}
void AffineSpace::check(std::size_t dimension) const {
  if (dimension_ != dimension) throw std::invalid_argument("affine dimension mismatch");
}
Matrix AffineSpace::representative() const {
  if (empty_) throw std::logic_error("empty affine space has no representative");
  return Matrix(dimension_, offset_);
}
bool AffineSpace::addDirection(BitVector direction) {
  if (empty_) throw std::logic_error("direction without affine representative");
  if (!basis_.insert(std::move(direction))) return false;
  offset_ = basis_.reduce(std::move(offset_));
  return true;
}
bool AffineSpace::addPoint(const Matrix &point) {
  check(point.dimension());
  if (empty_) { offset_ = point.entries(); empty_ = false; return true; }
  return addDirection(point.entries() ^ offset_);
}
bool AffineSpace::joinWith(const AffineSpace &other) {
  check(other.dimension_);
  if (this == &other || other.empty_) return false;
  if (empty_) { *this = other; return true; }
  bool changed = addDirection(other.offset_ ^ offset_);
  for (const auto &row : other.directions()) changed = addDirection(row) || changed;
  return changed;
}
bool AffineSpace::contains(const Matrix &point) const {
  check(point.dimension());
  return !empty_ && basis_.reduce(point.entries() ^ offset_).empty();
}
bool AffineSpace::contains(const AffineSpace &other) const {
  check(other.dimension_);
  if (other.empty_) return true;
  if (empty_ || !contains(other.representative())) return false;
  for (const auto &row : other.directions()) if (!basis_.reduce(row).empty()) return false;
  return true;
}
bool AffineSpace::intersects(const AffineSpace &other) const {
  check(other.dimension_);
  if (empty_ || other.empty_) return false;
  LinearBasis combined = basis_;
  for (const auto &row : other.directions()) combined.insert(row);
  return combined.reduce(offset_ ^ other.offset_).empty();
}
AffineSpace AffineSpace::product(const AffineSpace &right) const {
  check(right.dimension_);
  if (empty_ || right.empty_) return AffineSpace(dimension_);
  const auto a = representative(), b = right.representative();
  if (rank() == 0 && a.isIdentity()) return right;
  if (right.rank() == 0 && b.isIdentity()) return *this;
  AffineSpace result = singleton(a * b);
  // (a+U)(b+V) has affine hull ab + span(Ub, aV, UV).
  // The UV terms are essential, including when a=b=0.
  for (const auto &u : directions())
    result.addDirection((Matrix(dimension_, u) * b).entries());
  for (const auto &v : right.directions())
    result.addDirection((a * Matrix(dimension_, v)).entries());
  for (const auto &u : directions())
    for (const auto &v : right.directions())
      result.addDirection((Matrix(dimension_, u) * Matrix(dimension_, v)).entries());
  return result;
}
AffineSpace AffineSpace::block(std::size_t offset, std::size_t dimension) const {
  // Validate the block even for bottom.
  (void)Matrix(dimension_).block(offset, dimension);
  AffineSpace result(dimension);
  if (!empty_) {
    result.addPoint(representative().block(offset, dimension));
    for (const auto &u : directions())
      result.addDirection(Matrix(dimension_, u).block(offset, dimension).entries());
  }
  return result;
}
bool AffineSpace::operator==(const AffineSpace &other) const {
  return dimension_ == other.dimension_ && empty_ == other.empty_ &&
      offset_ == other.offset_ && directions() == other.directions();
}
bool SeparationCertificate::verify(const AffineSpace &left, const AffineSpace &right) const {
  if (left.empty() || right.empty() || left_value == right_value ||
      left.dimension() != functional.dimension() || right.dimension() != functional.dimension())
    return false;
  const auto &f = functional.entries();
  if (f.dot(left.offset()) != left_value || f.dot(right.offset()) != right_value) return false;
  for (const auto &row : left.directions()) if (f.dot(row)) return false;
  for (const auto &row : right.directions()) if (f.dot(row)) return false;
  return true;
}
std::optional<SeparationCertificate> separate(const AffineSpace &left, const AffineSpace &right) {
  if (left.dimension() != right.dimension()) throw std::invalid_argument("separation dimension mismatch");
  if (left.empty() || right.empty()) return std::nullopt;
  LinearBasis combined(left.offset().size());
  for (const auto &row : left.directions()) combined.insert(row);
  for (const auto &row : right.directions()) combined.insert(row);
  const auto residual = combined.reduce(left.offset() ^ right.offset());
  const auto free = residual.firstSet();
  if (free == residual.size()) return std::nullopt;
  BitVector functional(residual.size()); functional.set(free);
  // RREF makes all other pivot columns zero. Choose one free coordinate and
  // solve the orthogonality equations directly, without a second elimination.
  for (const auto &row : combined.rows()) functional.set(row.firstSet(), row.test(free));
  const bool l = functional.dot(left.offset()), r = functional.dot(right.offset());
  return SeparationCertificate{Matrix(left.dimension(), std::move(functional)), l, r};
}
AffineSemiring::AffineSemiring(std::size_t dimension)
    : dimension_(dimension), identity_(Matrix::identity(dimension)) {}
void AffineSemiring::check(const Weight &weight) const {
  if (weight.dimension() != dimension_) throw std::invalid_argument("semiring dimension mismatch");
}
AffineSemiring::Weight AffineSemiring::lift(const Matrix &matrix) const {
  if (matrix.dimension() != dimension_) throw std::invalid_argument("event matrix dimension mismatch");
  return AffineSpace::singleton(matrix);
}
AffineSemiring::Weight AffineSemiring::combine(const Weight &left, const Weight &right) const {
  check(left); check(right);
  Weight result = left; result.joinWith(right); return result;
}
AffineSemiring::Weight AffineSemiring::extend(const Weight &left, const Weight &right) const {
  check(left); check(right); return left.product(right);
}
} // namespace lotus::cfl::interleaved_dyck::affine

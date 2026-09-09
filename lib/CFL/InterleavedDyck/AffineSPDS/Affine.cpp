#include "CFL/InterleavedDyck/AffineSPDS/Affine.h"

#include <algorithm>
#include <stdexcept>

namespace lotus::cfl::interleaved_dyck::affine {
const llvm::SmallVectorImpl<BitVector> &LinearBasis::rows() const {
  static const llvm::SmallVector<BitVector, 0> empty;
  if (storage_)
    return storage_->rows;
  return empty;
}
const llvm::SmallVectorImpl<std::size_t> &LinearBasis::pivots() const {
  static const llvm::SmallVector<std::size_t, 0> empty;
  if (storage_)
    return storage_->pivots;
  return empty;
}
std::size_t LinearBasis::rank() const {
  return storage_ ? storage_->rows.size() : 0;
}
bool LinearBasis::operator==(const LinearBasis &other) const {
  return coordinates_ == other.coordinates_ &&
         (storage_ == other.storage_ ||
          (rows() == other.rows() && pivots() == other.pivots()));
}
LinearBasis::Storage &LinearBasis::write() {
  if (!storage_)
    storage_ = std::make_shared<Storage>();
  else if (storage_.use_count() != 1)
    storage_ = std::make_shared<Storage>(*storage_);
  return *storage_;
}
void LinearBasis::reduceInPlace(BitVector &vector) const {
  if (vector.size() != coordinates_)
    throw std::invalid_argument("basis dimension mismatch");
  const auto &basis_rows = rows();
  const auto &basis_pivots = pivots();
  for (std::size_t i = 0; i < basis_rows.size(); ++i) {
    const std::size_t pivot = basis_pivots[i];
    const std::uint64_t bits = vector.data()[pivot / 64];
    if ((bits >> (pivot % 64)) & 1U)
      vector ^= basis_rows[i];
  }
}
BitVector LinearBasis::reduce(BitVector vector) const {
  reduceInPlace(vector);
  return vector;
}
bool LinearBasis::insert(BitVector vector) {
  reduceInPlace(vector);
  const auto pivot = vector.firstSet();
  if (pivot == coordinates_)
    return false;
  auto &basis = write();
  for (auto &row : basis.rows) {
    const std::uint64_t bits = row.data()[pivot / 64];
    if ((bits >> (pivot % 64)) & 1U)
      row ^= vector;
  }
  const auto where =
      std::lower_bound(basis.pivots.begin(), basis.pivots.end(), pivot);
  const auto index = static_cast<std::size_t>(where - basis.pivots.begin());
  basis.pivots.insert(where, pivot);
  basis.rows.insert(basis.rows.begin() + index, std::move(vector));
  return true;
}
bool LinearBasis::insertWithPivot(BitVector vector, std::size_t &new_pivot) {
  reduceInPlace(vector);
  const auto pivot = vector.firstSet();
  if (pivot == coordinates_)
    return false;
  new_pivot = pivot;
  auto &basis = write();
  for (auto &row : basis.rows) {
    const std::uint64_t bits = row.data()[pivot / 64];
    if ((bits >> (pivot % 64)) & 1U)
      row ^= vector;
  }
  const auto where =
      std::lower_bound(basis.pivots.begin(), basis.pivots.end(), pivot);
  const auto index = static_cast<std::size_t>(where - basis.pivots.begin());
  basis.pivots.insert(where, pivot);
  basis.rows.insert(basis.rows.begin() + index, std::move(vector));
  return true;
}
bool LinearBasis::insertBatch(llvm::SmallVectorImpl<BitVector> &vectors) {
  bool changed = false;
  for (auto &input : vectors)
    changed = insertEchelon(std::move(input)) || changed;
  if (!changed)
    return false;
  canonicalize();
  return true;
}
bool LinearBasis::insertEchelon(BitVector vector) {
  reduceInPlace(vector);
  const auto pivot = vector.firstSet();
  if (pivot == coordinates_)
    return false;
  auto &basis = write();
  const auto where =
      std::lower_bound(basis.pivots.begin(), basis.pivots.end(), pivot);
  const auto index = static_cast<std::size_t>(where - basis.pivots.begin());
  basis.pivots.insert(where, pivot);
  basis.rows.insert(basis.rows.begin() + index, std::move(vector));
  return true;
}
bool LinearBasis::insertEchelonWithPivot(BitVector vector,
                                         std::size_t &new_pivot) {
  reduceInPlace(vector);
  const auto pivot = vector.firstSet();
  if (pivot == coordinates_)
    return false;
  new_pivot = pivot;
  auto &basis = write();
  const auto where =
      std::lower_bound(basis.pivots.begin(), basis.pivots.end(), pivot);
  const auto index = static_cast<std::size_t>(where - basis.pivots.begin());
  basis.pivots.insert(where, pivot);
  basis.rows.insert(basis.rows.begin() + index, std::move(vector));
  return true;
}
void LinearBasis::canonicalize() {
  auto &basis = write();
  for (std::size_t i = basis.rows.size(); i-- > 0;) {
    const std::size_t pivot = basis.pivots[i];
    for (std::size_t j = 0; j < i; ++j) {
      const std::uint64_t bits = basis.rows[j].data()[pivot / 64];
      if ((bits >> (pivot % 64)) & 1U)
        basis.rows[j] ^= basis.rows[i];
    }
  }
}
AffineSpace::AffineSpace(std::size_t dimension)
    : dimension_(dimension), offset_(Matrix(dimension).coordinates()),
      basis_(offset_.size()) {}
AffineSpace AffineSpace::singleton(const Matrix &point) {
  AffineSpace result(point.dimension());
  result.addPoint(point);
  return result;
}
AffineSpace AffineSpace::top(std::size_t dimension) {
  AffineSpace result = singleton(Matrix(dimension));
  for (std::size_t i = 0; i < result.offset_.size(); ++i) {
    BitVector unit(result.offset_.size());
    unit.set(i);
    result.addDirection(std::move(unit));
  }
  return result;
}
void AffineSpace::check(std::size_t dimension) const {
  if (dimension_ != dimension)
    throw std::invalid_argument("affine dimension mismatch");
}
Matrix AffineSpace::representative() const {
  if (empty_)
    throw std::logic_error("empty affine space has no representative");
  return Matrix(dimension_, offset_);
}
bool AffineSpace::addDirection(BitVector direction) {
  if (empty_)
    throw std::logic_error("direction without affine representative");
  if (!basis_.insert(std::move(direction)))
    return false;
  is_identity_ = false;
  basis_.reduceInPlace(offset_);
  return true;
}
bool AffineSpace::addPoint(const Matrix &point) {
  check(point.dimension());
  if (empty_) {
    offset_ = point.entries();
    empty_ = false;
    is_identity_ = point.isIdentity();
    return true;
  }
  return addDirection(point.entries() ^ offset_);
}
bool AffineSpace::joinWith(const AffineSpace &other) {
  check(other.dimension_);
  if (this == &other || other.empty_)
    return false;
  if (empty_) {
    *this = other;
    return true;
  }
  if (rank() == 0 && other.rank() == 0 && offset_ == other.offset_)
    return false;
  bool changed = basis_.insert(other.offset_ ^ offset_);
  for (const auto &row : other.basis_.rows())
    changed = basis_.insert(row) || changed;
  if (changed) {
    is_identity_ = false;
    basis_.reduceInPlace(offset_);
  }
  return changed;
}
bool AffineSpace::joinWithDelta(const AffineSpace &other,
                                AffineDelta *delta) {
  if (!delta)
    return joinWith(other);
  check(other.dimension_);
  if (this == &other || other.empty_)
    return false;
  if (empty_) {
    *this = other;
    delta->pivots.clear();
    delta->full = true;
    return true;
  }
  if (rank() == 0 && other.rank() == 0 && offset_ == other.offset_)
    return false;
  // Saturation joins are incremental and generally low-rank. Insert directly
  // and canonicalize the representative once, without a temporary batch.
  std::size_t pivot;
  bool changed = basis_.insertWithPivot(other.offset_ ^ offset_, pivot);
  if (changed)
    delta->pivots.push_back(pivot);
  for (const auto &row : other.basis_.rows()) {
    const bool inserted = basis_.insertWithPivot(row, pivot);
    if (inserted)
      delta->pivots.push_back(pivot);
    changed = inserted || changed;
  }
  if (changed) {
    is_identity_ = false;
    basis_.reduceInPlace(offset_);
  }
  return changed;
}
bool AffineSpace::contains(const Matrix &point) const {
  check(point.dimension());
  return !empty_ && basis_.reduce(point.entries() ^ offset_).empty();
}
bool AffineSpace::contains(const AffineSpace &other) const {
  check(other.dimension_);
  if (other.empty_)
    return true;
  if (empty_ || !contains(other.representative()))
    return false;
  for (const auto &row : other.basis_.rows())
    if (!basis_.reduce(row).empty())
      return false;
  return true;
}
bool AffineSpace::intersects(const AffineSpace &other) const {
  check(other.dimension_);
  if (empty_ || other.empty_)
    return false;
  LinearBasis combined = basis_;
  for (const auto &row : other.basis_.rows())
    combined.insert(row);
  return combined.reduce(offset_ ^ other.offset_).empty();
}
bool AffineSpace::isIdentity() const { return is_identity_; }
AffineSpace AffineSpace::product(const AffineSpace &right) const {
  AffineSpace result(dimension_);
  result.joinProduct(*this, right);
  return result;
}
bool AffineSpace::joinProduct(const AffineSpace &left,
                              const AffineSpace &right) {
  return joinProduct(left, right, nullptr);
}
bool AffineSpace::joinProduct(
    const AffineSpace &left, const AffineSpace &right,
    const RightMatrixMultiplier *prepared_right) {
  check(left.dimension_);
  check(right.dimension_);
  if (left.empty_ || right.empty_)
    return false;
  if (this == &left || this == &right) {
    const auto extended = left.product(right);
    return joinWith(extended);
  }
  if (rank() == offset_.size())
    return false;
  if (left.isIdentity())
    return joinWith(right);
  if (right.isIdentity())
    return joinWith(left);
  bool changed = false, basis_changed = false;
  auto full = [&] { return rank() == offset_.size(); };
  auto add = [&](BitVector direction) {
    const bool inserted = basis_.insertEchelon(std::move(direction));
    basis_changed = inserted || basis_changed;
    changed = inserted || changed;
  };
  auto add_point = [&](BitVector point, bool identity) {
    if (empty_) {
      offset_ = std::move(point);
      empty_ = false;
      is_identity_ = identity;
      changed = true;
    } else {
      add(point ^ offset_);
    }
  };
  auto finish = [&] {
    if (basis_changed) {
      basis_.canonicalize();
      is_identity_ = false;
      basis_.reduceInPlace(offset_);
    }
    return changed;
  };
  const BitVector &a = left.offset_;
  std::optional<RightMatrixMultiplier> local_multiplier;
  const RightMatrixMultiplier *multiply_by_b = prepared_right;
  if (!multiply_by_b) {
    local_multiplier.emplace(dimension_, right.offset_);
    multiply_by_b = &*local_multiplier;
  }
  BitVector point = multiply_by_b->multiply(a);
  const bool point_is_identity =
      empty_ && point.isIdentityMatrix(dimension_);
  add_point(std::move(point), point_is_identity);
  const std::size_t left_rank = left.rank(), right_rank = right.rank();
  if (full() || (left_rank == 0 && right_rank == 0))
    return finish();
  for (const auto &u : left.basis_.rows()) {
    if (full())
      break;
    add(multiply_by_b->multiply(u));
  }
  if (full() || right_rank == 0)
    return finish();
  for (const auto &direction : right.basis_.rows()) {
    if (full())
      break;
    const RightMatrixMultiplier multiply_by_v(dimension_, direction);
    add(multiply_by_v.multiply(a));
    for (const auto &u : left.basis_.rows()) {
      if (full())
        break;
      add(multiply_by_v.multiply(u));
    }
  }
  return finish();
}
bool AffineSpace::joinProductWithDelta(
    const AffineSpace &left, const AffineSpace &right,
    const RightMatrixMultiplier *prepared_right, AffineDelta *delta) {
  check(left.dimension_);
  check(right.dimension_);
  if (left.empty_ || right.empty_)
    return false;
  if (this == &left || this == &right) {
    const auto extended = left.product(right);
    return joinWithDelta(extended, delta);
  }
  if (rank() == offset_.size())
    return false;
  if (left.isIdentity())
    return joinWithDelta(right, delta);
  if (right.isIdentity())
    return joinWithDelta(left, delta);
  const bool was_empty = empty_;
  bool changed = false, basis_changed = false;
  auto full = [&] { return rank() == offset_.size(); };
  auto add = [&](BitVector direction) {
    std::size_t pivot;
    const bool inserted =
        basis_.insertEchelonWithPivot(std::move(direction), pivot);
    if (inserted)
      delta->pivots.push_back(pivot);
    basis_changed = inserted || basis_changed;
    changed = inserted || changed;
  };
  auto add_point = [&](BitVector point, bool identity) {
    if (empty_) {
      offset_ = std::move(point);
      empty_ = false;
      is_identity_ = identity;
      changed = true;
    } else {
      add(point ^ offset_);
    }
  };
  auto finish = [&] {
    if (basis_changed) {
      basis_.canonicalize();
      is_identity_ = false;
      basis_.reduceInPlace(offset_);
    }
    if (changed && was_empty) {
      delta->pivots.clear();
      delta->full = true;
    }
    return changed;
  };
  // (a+U)(b+V) has affine hull ab + span(Ub, aV, UV).
  // The UV terms are essential, including when a=b=0.
  const BitVector &a = left.offset_;
  std::optional<RightMatrixMultiplier> local_multiplier;
  const RightMatrixMultiplier *multiply_by_b = prepared_right;
  if (!multiply_by_b) {
    local_multiplier.emplace(dimension_, right.offset_);
    multiply_by_b = &*local_multiplier;
  }
  BitVector point = multiply_by_b->multiply(a);
  const bool point_is_identity =
      empty_ && point.isIdentityMatrix(dimension_);
  add_point(std::move(point), point_is_identity);
  const std::size_t left_rank = left.rank(), right_rank = right.rank();
  if (full() || (left_rank == 0 && right_rank == 0))
    return finish();
  for (const auto &u : left.basis_.rows()) {
    if (full())
      break;
    add(multiply_by_b->multiply(u));
  }
  if (full() || right_rank == 0)
    return finish();
  for (const auto &direction : right.basis_.rows()) {
    if (full())
      break;
    const RightMatrixMultiplier multiply_by_v(dimension_, direction);
    add(multiply_by_v.multiply(a));
    for (const auto &u : left.basis_.rows()) {
      if (full())
        break;
      add(multiply_by_v.multiply(u));
    }
  }
  return finish();
}
bool AffineSpace::joinProductDelta(
    const AffineSpace &left, const AffineSpace &right,
    const AffineDelta &input, bool input_is_left, AffineDelta *output,
    const RightMatrixMultiplier *prepared_right) {
  check(left.dimension_);
  check(right.dimension_);
  if (input.empty() || left.empty_ || right.empty_)
    return false;
  if (input.full)
    return output ? joinProductWithDelta(left, right, prepared_right, output)
                  : joinProduct(left, right, prepared_right);
  if (empty_)
    return output ? joinProductWithDelta(left, right, prepared_right, output)
                  : joinProduct(left, right, prepared_right);
  if (rank() == offset_.size())
    return false;
  bool changed = false;
  auto add = [&](BitVector direction) {
    std::size_t pivot;
    const bool inserted = output
                              ? basis_.insertWithPivot(std::move(direction),
                                                       pivot)
                              : basis_.insert(std::move(direction));
    if (inserted && output)
      output->pivots.push_back(pivot);
    changed = inserted || changed;
  };
  auto forEachInputDirection = [&](const AffineSpace &space,
                                   const auto &function) {
    const auto &pivots = space.basis_.pivots();
    const auto &rows = space.basis_.rows();
    for (std::size_t pivot : input.pivots) {
      const auto found = std::lower_bound(pivots.begin(), pivots.end(), pivot);
      if (found == pivots.end() || *found != pivot)
        throw std::logic_error("affine delta pivot is absent from input");
      function(rows[static_cast<std::size_t>(found - pivots.begin())]);
    }
  };
  if (input_is_left) {
    std::optional<RightMatrixMultiplier> local_multiplier;
    const RightMatrixMultiplier *multiply_by_offset = prepared_right;
    if (!multiply_by_offset) {
      local_multiplier.emplace(dimension_, right.offset_);
      multiply_by_offset = &*local_multiplier;
    }
    forEachInputDirection(left, [&](const BitVector &direction) {
      add(multiply_by_offset->multiply(direction));
      for (const auto &right_direction : right.basis_.rows()) {
        const RightMatrixMultiplier multiply_by_direction(
            dimension_, right_direction);
        add(multiply_by_direction.multiply(direction));
      }
    });
  } else {
    forEachInputDirection(right, [&](const BitVector &direction) {
      const RightMatrixMultiplier multiply_by_direction(dimension_, direction);
      add(multiply_by_direction.multiply(left.offset_));
      for (const auto &left_direction : left.basis_.rows())
        add(multiply_by_direction.multiply(left_direction));
    });
  }
  if (changed) {
    is_identity_ = false;
    basis_.reduceInPlace(offset_);
  }
  return changed;
}
bool AffineSpace::joinTripleProductDelta(
    const AffineSpace &left, const AffineSpace &middle,
    const AffineSpace &right, const AffineDelta &input,
    bool input_is_middle, AffineDelta *output) {
  check(left.dimension_);
  check(middle.dimension_);
  check(right.dimension_);
  if (input.empty() || left.empty_ || middle.empty_ || right.empty_)
    return false;
  if (input.full || empty_) {
    const AffineSpace continuation = middle.product(right);
    return output ? joinProductWithDelta(left, continuation, nullptr, output)
                  : joinProduct(left, continuation, nullptr);
  }
  if (rank() == offset_.size())
    return false;
  bool changed = false;
  auto full = [&] { return rank() == offset_.size(); };
  auto add = [&](BitVector direction) {
    if (full())
      return;
    std::size_t pivot;
    const bool inserted = output
                              ? basis_.insertWithPivot(std::move(direction),
                                                       pivot)
                              : basis_.insert(std::move(direction));
    if (inserted && output)
      output->pivots.push_back(pivot);
    changed = inserted || changed;
  };
  auto forEachInputDirection = [&](const AffineSpace &space,
                                   const auto &function) {
    const auto &pivots = space.basis_.pivots();
    const auto &rows = space.basis_.rows();
    for (std::size_t pivot : input.pivots) {
      const auto found = std::lower_bound(pivots.begin(), pivots.end(), pivot);
      if (found == pivots.end() || *found != pivot)
        throw std::logic_error("affine delta pivot is absent from input");
      function(rows[static_cast<std::size_t>(found - pivots.begin())]);
      if (full())
        break;
    }
  };
  if (input_is_middle) {
    const RightMatrixMultiplier multiply_by_right_offset(
        dimension_, right.offset_);
    forEachInputDirection(middle, [&](const BitVector &direction) {
      const RightMatrixMultiplier multiply_by_delta(dimension_, direction);
      llvm::SmallVector<BitVector, 2> partials;
      partials.push_back(multiply_by_delta.multiply(left.offset_));
      for (const auto &left_direction : left.basis_.rows())
        partials.push_back(multiply_by_delta.multiply(left_direction));
      for (const auto &partial : partials)
        add(multiply_by_right_offset.multiply(partial));
      for (const auto &right_direction : right.basis_.rows()) {
        if (full())
          break;
        const RightMatrixMultiplier multiply_by_right_direction(
            dimension_, right_direction);
        for (const auto &partial : partials)
          add(multiply_by_right_direction.multiply(partial));
      }
    });
  } else {
    const AffineSpace prefix = left.product(middle);
    forEachInputDirection(right, [&](const BitVector &direction) {
      const RightMatrixMultiplier multiply_by_delta(dimension_, direction);
      add(multiply_by_delta.multiply(prefix.offset_));
      for (const auto &prefix_direction : prefix.basis_.rows())
        add(multiply_by_delta.multiply(prefix_direction));
    });
  }
  if (changed) {
    is_identity_ = false;
    basis_.reduceInPlace(offset_);
  }
  return changed;
}
AffineSpace AffineSpace::block(std::size_t offset,
                               std::size_t dimension) const {
  // Validate the block even for bottom.
  (void)Matrix(dimension_).block(offset, dimension);
  AffineSpace result(dimension);
  if (!empty_) {
    result.addPoint(representative().block(offset, dimension));
    for (const auto &u : basis_.rows())
      result.addDirection(
          Matrix(dimension_, u).block(offset, dimension).entries());
  }
  return result;
}
bool AffineSpace::operator==(const AffineSpace &other) const {
  if (dimension_ != other.dimension_ || empty_ != other.empty_)
    return false;
  return empty_ ||
         (offset_ == other.offset_ && basis_ == other.basis_);
}
bool SeparationCertificate::verify(const AffineSpace &left,
                                   const AffineSpace &right) const {
  if (left.empty() || right.empty() || left_value == right_value ||
      left.dimension() != functional.dimension() ||
      right.dimension() != functional.dimension())
    return false;
  const auto &f = functional.entries();
  if (f.dot(left.offset()) != left_value ||
      f.dot(right.offset()) != right_value)
    return false;
  for (const auto &row : left.directions())
    if (f.dot(row))
      return false;
  for (const auto &row : right.directions())
    if (f.dot(row))
      return false;
  return true;
}
std::optional<SeparationCertificate> separate(const AffineSpace &left,
                                              const AffineSpace &right) {
  if (left.dimension() != right.dimension())
    throw std::invalid_argument("separation dimension mismatch");
  if (left.empty() || right.empty())
    return std::nullopt;
  LinearBasis combined(left.offset().size());
  for (const auto &row : left.directions())
    combined.insert(row);
  for (const auto &row : right.directions())
    combined.insert(row);
  const auto residual = combined.reduce(left.offset() ^ right.offset());
  const auto free = residual.firstSet();
  if (free == residual.size())
    return std::nullopt;
  BitVector functional(residual.size());
  functional.set(free);
  // RREF makes all other pivot columns zero. Choose one free coordinate and
  // solve the orthogonality equations directly, without a second elimination.
  for (std::size_t i = 0; i < combined.rows().size(); ++i)
    functional.set(combined.pivots()[i], combined.rows()[i].test(free));
  const bool l = functional.dot(left.offset()),
             r = functional.dot(right.offset());
  return SeparationCertificate{Matrix(left.dimension(), std::move(functional)),
                               l, r};
}
AffineSemiring::AffineSemiring(std::size_t dimension)
    : dimension_(dimension), identity_(Matrix::identity(dimension)),
      zero_(dimension), one_(AffineSpace::singleton(identity_)) {}
void AffineSemiring::check(const Weight &weight) const {
  if (weight.dimension() != dimension_)
    throw std::invalid_argument("semiring dimension mismatch");
}
AffineSemiring::Weight AffineSemiring::lift(const Matrix &matrix) const {
  if (matrix.dimension() != dimension_)
    throw std::invalid_argument("event matrix dimension mismatch");
  return AffineSpace::singleton(matrix);
}
AffineSemiring::Weight AffineSemiring::combine(const Weight &left,
                                               const Weight &right) const {
  check(left);
  check(right);
  Weight result = left;
  result.joinWith(right);
  return result;
}
bool AffineSemiring::combineWith(Weight &left, const Weight &right) const {
  check(left);
  check(right);
  return left.joinWith(right);
}
bool AffineSemiring::combineWithDelta(Weight &left, const Weight &right,
                                     Delta *delta) const {
  check(left);
  check(right);
  return left.joinWithDelta(right, delta);
}
AffineSemiring::Weight AffineSemiring::extend(const Weight &left,
                                              const Weight &right) const {
  check(left);
  check(right);
  return left.product(right);
}
bool AffineSemiring::extendAndCombine(Weight &target, const Weight &left,
                                     const Weight &right) const {
  check(target);
  check(left);
  check(right);
  return target.joinProduct(left, right);
}
bool AffineSemiring::extendAndCombineDelta(Weight &target,
                                          const Weight &left,
                                          const Weight &right,
                                          Delta *delta) const {
  check(target);
  check(left);
  check(right);
  return target.joinProductWithDelta(left, right, nullptr, delta);
}
bool AffineSemiring::extendDeltaAndCombine(
    Weight &target, const Weight &left, const Weight &right,
    const Delta &input, bool input_is_left, Delta *output) const {
  check(target);
  check(left);
  check(right);
  return target.joinProductDelta(left, right, input, input_is_left, output);
}
AffineSemiring::PreparedWeight
AffineSemiring::prepareWeight(const Weight &weight) const {
  check(weight);
  if (weight.empty() || weight.rank() != 0 || weight.isIdentity() ||
      dimension_ > 64)
    return nullptr;
  auto prepared =
      std::make_shared<RightMatrixMultiplier>(dimension_, weight.offset());
  return prepared;
}
AffineSemiring::Weight AffineSemiring::extendPrepared(
    const Weight &left, const Weight &right,
    const PreparedWeight &prepared_right) const {
  check(left);
  check(right);
  if (!prepared_right)
    return left.product(right);
  Weight result(dimension_);
  result.joinProduct(left, right, prepared_right.get());
  return result;
}
bool AffineSemiring::extendAndCombinePrepared(
    Weight &target, const Weight &left, const Weight &right,
    const PreparedWeight &prepared_right) const {
  check(target);
  check(left);
  check(right);
  return prepared_right
             ? target.joinProduct(left, right, prepared_right.get())
             : target.joinProduct(left, right);
}
bool AffineSemiring::extendAndCombinePreparedDelta(
    Weight &target, const Weight &left, const Weight &right,
    const PreparedWeight &prepared_right, Delta *delta) const {
  check(target);
  check(left);
  check(right);
  return prepared_right
             ? target.joinProductWithDelta(left, right, prepared_right.get(),
                                           delta)
             : target.joinProductWithDelta(left, right, nullptr, delta);
}
bool AffineSemiring::extendPreparedInputDeltaAndCombine(
    Weight &target, const Weight &left, const Weight &right,
    const PreparedWeight &prepared_right, const Delta &input,
    Delta *output) const {
  check(target);
  check(left);
  check(right);
  return target.joinProductDelta(left, right, input, true, output,
                                 prepared_right.get());
}
bool AffineSemiring::extendPushDeltaAndCombine(
    Weight &target, const Weight &rule, const Weight &first,
    const Weight &second, const Delta &input, bool input_is_first,
    Delta *output) const {
  check(target);
  check(rule);
  check(first);
  check(second);
  return target.joinTripleProductDelta(rule, first, second, input,
                                       input_is_first, output);
}
void AffineSemiring::mergeDelta(Delta &target, Delta source) const {
  if (target.full)
    return;
  if (source.full) {
    target.pivots.clear();
    target.full = true;
    return;
  }
  target.pivots.reserve(target.pivots.size() + source.pivots.size());
  target.pivots.append(source.pivots.begin(), source.pivots.end());
}
} // namespace lotus::cfl::interleaved_dyck::affine

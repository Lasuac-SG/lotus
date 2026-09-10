#include "CFL/InterleavedDyck/AffineSPDS/Affine.h"

#include <algorithm>
#include <chrono>
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
LinearBasis::Storage &LinearBasis::write(AlgebraStatistics *statistics) {
  if (!storage_)
    storage_ = std::make_shared<Storage>();
  else if (storage_.use_count() != 1) {
    if (statistics)
      ++statistics->cow_detaches;
    storage_ = std::make_shared<Storage>(*storage_);
  }
  return *storage_;
}
void LinearBasis::reduceInPlace(BitVector &vector,
                                AlgebraStatistics *statistics) const {
  if (statistics)
    ++statistics->basis_reductions;
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
BitVector LinearBasis::reduce(BitVector vector,
                              AlgebraStatistics *statistics) const {
  reduceInPlace(vector, statistics);
  return vector;
}
bool LinearBasis::insert(BitVector vector, AlgebraStatistics *statistics) {
  reduceInPlace(vector, statistics);
  const auto pivot = vector.firstSet();
  if (pivot == coordinates_)
    return false;
  if (statistics)
    ++statistics->basis_insertions;
  auto &basis = write(statistics);
  for (auto &row : basis.rows) {
    const std::uint64_t bits = row.data()[pivot / 64];
    if ((bits >> (pivot % 64)) & 1U)
      row ^= vector;
  }
  auto *const where =
      std::lower_bound(basis.pivots.begin(), basis.pivots.end(), pivot);
  const auto index = static_cast<std::size_t>(where - basis.pivots.begin());
  basis.pivots.insert(where, pivot);
  basis.rows.insert(basis.rows.begin() + index, std::move(vector));
  return true;
}
bool LinearBasis::insertWithPivot(BitVector vector, std::size_t &new_pivot,
                                  AlgebraStatistics *statistics) {
  reduceInPlace(vector, statistics);
  const auto pivot = vector.firstSet();
  if (pivot == coordinates_)
    return false;
  new_pivot = pivot;
  if (statistics)
    ++statistics->basis_insertions;
  auto &basis = write(statistics);
  for (auto &row : basis.rows) {
    const std::uint64_t bits = row.data()[pivot / 64];
    if ((bits >> (pivot % 64)) & 1U)
      row ^= vector;
  }
  auto *const where =
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
bool LinearBasis::insertEchelon(BitVector vector,
                                AlgebraStatistics *statistics) {
  reduceInPlace(vector, statistics);
  const auto pivot = vector.firstSet();
  if (pivot == coordinates_)
    return false;
  if (statistics)
    ++statistics->basis_insertions;
  auto &basis = write(statistics);
  auto *const where =
      std::lower_bound(basis.pivots.begin(), basis.pivots.end(), pivot);
  const auto index = static_cast<std::size_t>(where - basis.pivots.begin());
  basis.pivots.insert(where, pivot);
  basis.rows.insert(basis.rows.begin() + index, std::move(vector));
  return true;
}
bool LinearBasis::insertEchelonWithPivot(BitVector vector,
                                         std::size_t &new_pivot,
                                         AlgebraStatistics *statistics) {
  reduceInPlace(vector, statistics);
  const auto pivot = vector.firstSet();
  if (pivot == coordinates_)
    return false;
  new_pivot = pivot;
  if (statistics)
    ++statistics->basis_insertions;
  auto &basis = write(statistics);
  auto *const where =
      std::lower_bound(basis.pivots.begin(), basis.pivots.end(), pivot);
  const auto index = static_cast<std::size_t>(where - basis.pivots.begin());
  basis.pivots.insert(where, pivot);
  basis.rows.insert(basis.rows.begin() + index, std::move(vector));
  return true;
}
void LinearBasis::canonicalize(AlgebraStatistics *statistics) {
  auto &basis = write(statistics);
  for (std::size_t i = basis.rows.size(); i-- > 0;) {
    const std::size_t pivot = basis.pivots[i];
    for (std::size_t j = 0; j < i; ++j) {
      const std::uint64_t bits = basis.rows[j].data()[pivot / 64];
      if ((bits >> (pivot % 64)) & 1U)
        basis.rows[j] ^= basis.rows[i];
    }
  }
}
AffineSpace::AffineSpace(std::size_t dimension,
                         std::shared_ptr<const MatrixLayout> layout)
    : dimension_(dimension), layout_(std::move(layout)),
      offset_(layout_ ? layout_->coordinates()
                      : Matrix::coordinateCount(dimension)),
      basis_(offset_.size()) {
  if (layout_ && layout_->dimension() != dimension)
    throw std::invalid_argument("affine layout dimension mismatch");
}
AffineSpace AffineSpace::singleton(const Matrix &point,
                                   std::shared_ptr<const MatrixLayout> layout) {
  AffineSpace result(point.dimension(), std::move(layout));
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
  return decodeEntries(offset_);
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
    offset_ = layout_ ? layout_->pack(point) : point.entries();
    empty_ = false;
    is_identity_ = point.isIdentity();
    return true;
  }
  return addDirection((layout_ ? layout_->pack(point) : point.entries()) ^
                      offset_);
}
bool AffineSpace::joinWith(const AffineSpace &other,
                           AlgebraStatistics *statistics) {
  check(other.dimension_);
  if (!compatibleLayout(other)) {
    *this = expanded();
    return joinWithDelta(other.expanded(), nullptr, statistics);
  }
  if (this == &other || other.empty_)
    return false;
  if (empty_) {
    *this = other;
    return true;
  }
  if (rank() == coordinates() ||
      (offset_ == other.offset_ && basis_ == other.basis_))
    return false;
  bool changed = basis_.insert(other.offset_ ^ offset_, statistics);
  for (const auto &row : other.basis_.rows())
    changed = basis_.insert(row, statistics) || changed;
  if (changed) {
    is_identity_ = false;
    basis_.reduceInPlace(offset_, statistics);
  }
  return changed;
}
bool AffineSpace::joinWithDelta(const AffineSpace &other, AffineDelta *delta,
                                AlgebraStatistics *statistics) {
  if (!delta)
    return joinWith(other, statistics);
  check(other.dimension_);
  if (!compatibleLayout(other)) {
    *this = expanded();
    return joinWithDelta(other.expanded(), delta, statistics);
  }
  if (this == &other || other.empty_)
    return false;
  if (empty_) {
    *this = other;
    delta->pivots.clear();
    delta->full = true;
    return true;
  }
  if (rank() == coordinates() ||
      (offset_ == other.offset_ && basis_ == other.basis_))
    return false;
  // Saturation joins are incremental and generally low-rank. Insert directly
  // and canonicalize the representative once, without a temporary batch.
  std::size_t pivot;
  bool changed =
      basis_.insertWithPivot(other.offset_ ^ offset_, pivot, statistics);
  if (changed)
    delta->pivots.push_back(pivot);
  for (const auto &row : other.basis_.rows()) {
    const bool inserted = basis_.insertWithPivot(row, pivot, statistics);
    if (inserted)
      delta->pivots.push_back(pivot);
    changed = inserted || changed;
  }
  if (changed) {
    is_identity_ = false;
    basis_.reduceInPlace(offset_, statistics);
  }
  return changed;
}
bool AffineSpace::contains(const Matrix &point) const {
  check(point.dimension());
  if (empty_ || (layout_ && !layout_->contains(point)))
    return false;
  return basis_
      .reduce((layout_ ? layout_->pack(point) : point.entries()) ^ offset_)
      .empty();
}
bool AffineSpace::contains(const AffineSpace &other) const {
  check(other.dimension_);
  if (other.empty_)
    return true;
  if (!compatibleLayout(other))
    return expanded().contains(other.expanded());
  if (empty_ || !contains(other.representative()))
    return false;
  for (const auto &row : other.basis_.rows())
    if (!basis_.reduce(row).empty())
      return false;
  return true;
}
bool AffineSpace::compatibleLayout(const AffineSpace &other) const {
  return layout_ == other.layout_ ||
         (layout_ && other.layout_ && *layout_ == *other.layout_);
}
Matrix AffineSpace::decodeEntries(const BitVector &entries) const {
  return layout_ ? layout_->unpack(entries) : Matrix(dimension_, entries);
}
AffineSpace AffineSpace::expanded() const {
  if (!layout_)
    return *this;
  AffineSpace result(dimension_);
  if (!empty_) {
    result.addPoint(representative());
    for (const auto &row : basis_.rows())
      result.addDirection(layout_->unpack(row).entries());
  }
  return result;
}
bool AffineSpace::identityPoint(const BitVector &point) const {
  return layout_ ? point == layout_->identity()
                 : point.isIdentityMatrix(dimension_);
}
bool AffineSpace::intersects(const AffineSpace &other,
                             AlgebraStatistics *statistics) const {
  check(other.dimension_);
  if (statistics)
    ++statistics->intersection_tests;
  if (empty_ || other.empty_)
    return false;
  if (!compatibleLayout(other))
    return expanded().intersects(other.expanded(), statistics);
  if (offset_ == other.offset_ || rank() == coordinates() ||
      other.rank() == coordinates()) {
    if (statistics)
      ++statistics->intersection_fast_paths;
    return true;
  }
  auto residual = basis_.reduce(offset_ ^ other.offset_, statistics);
  if (residual.empty()) {
    if (statistics)
      ++statistics->intersection_fast_paths;
    return true;
  }
  LinearBasis combined = basis_;
  for (const auto &row : other.basis_.rows()) {
    if (combined.insertEchelon(row, statistics)) {
      combined.reduceInPlace(residual, statistics);
      if (residual.empty())
        return true;
    }
  }
  return false;
}
bool AffineSpace::isIdentity() const { return is_identity_; }
AffineSpace AffineSpace::product(const AffineSpace &right,
                                 AlgebraStatistics *statistics) const {
  if (!compatibleLayout(right))
    return expanded().product(right.expanded(), statistics);
  AffineSpace result(dimension_, layout_);
  result.joinProduct(*this, right, nullptr, statistics);
  return result;
}
bool AffineSpace::joinProduct(const AffineSpace &left,
                              const AffineSpace &right) {
  return joinProduct(left, right, nullptr);
}
bool AffineSpace::joinProduct(const AffineSpace &left, const AffineSpace &right,
                              const RightMatrixMultiplier *prepared_right,
                              AlgebraStatistics *statistics) {
  check(left.dimension_);
  check(right.dimension_);
  if (!compatibleLayout(left) || !compatibleLayout(right))
    return joinWith(left.expanded().product(right.expanded(), statistics),
                    statistics);
  if (left.empty_ || right.empty_)
    return false;
  if (rank() == offset_.size())
    return false;
  if (this == &left || this == &right) {
    const auto extended = left.product(right, statistics);
    return joinWith(extended, statistics);
  }
  if (rank() == offset_.size())
    return false;
  if (left.isIdentity())
    return joinWith(right, statistics);
  if (right.isIdentity())
    return joinWith(left, statistics);
  bool changed = false, basis_changed = false;
  auto full = [&] { return rank() == offset_.size(); };
  auto add = [&](BitVector direction) {
    const bool inserted =
        basis_.insertEchelon(std::move(direction), statistics);
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
      basis_.canonicalize(statistics);
      is_identity_ = false;
      basis_.reduceInPlace(offset_, statistics);
    }
    return changed;
  };
  const BitVector &a = left.offset_;
  std::optional<RightMatrixMultiplier> local_multiplier;
  const RightMatrixMultiplier *multiply_by_b = prepared_right;
  if (!multiply_by_b) {
    local_multiplier.emplace(dimension_, right.offset_, layout_);
    multiply_by_b = &*local_multiplier;
  }
  BitVector point = multiply_by_b->multiply(a, statistics);
  const bool point_is_identity = empty_ && identityPoint(point);
  add_point(std::move(point), point_is_identity);
  const std::size_t left_rank = left.rank(), right_rank = right.rank();
  if (full() || (left_rank == 0 && right_rank == 0))
    return finish();
  for (const auto &u : left.basis_.rows()) {
    if (full())
      break;
    add(multiply_by_b->multiply(u, statistics));
  }
  if (full() || right_rank == 0)
    return finish();
  for (const auto &direction : right.basis_.rows()) {
    if (full())
      break;
    const RightMatrixMultiplier multiply_by_v(dimension_, direction, layout_);
    add(multiply_by_v.multiply(a, statistics));
    for (const auto &u : left.basis_.rows()) {
      if (full())
        break;
      add(multiply_by_v.multiply(u, statistics));
    }
  }
  return finish();
}
bool AffineSpace::joinProductWithDelta(
    const AffineSpace &left, const AffineSpace &right,
    const RightMatrixMultiplier *prepared_right, AffineDelta *delta,
    AlgebraStatistics *statistics) {
  check(left.dimension_);
  check(right.dimension_);
  if (!compatibleLayout(left) || !compatibleLayout(right))
    return joinWithDelta(left.expanded().product(right.expanded(), statistics),
                         delta, statistics);
  if (left.empty_ || right.empty_)
    return false;
  if (rank() == offset_.size())
    return false;
  if (this == &left || this == &right) {
    const auto extended = left.product(right, statistics);
    return joinWithDelta(extended, delta, statistics);
  }
  if (rank() == offset_.size())
    return false;
  if (left.isIdentity())
    return joinWithDelta(right, delta, statistics);
  if (right.isIdentity())
    return joinWithDelta(left, delta, statistics);
  const bool was_empty = empty_;
  bool changed = false, basis_changed = false;
  auto full = [&] { return rank() == offset_.size(); };
  auto add = [&](BitVector direction) {
    std::size_t pivot;
    const bool inserted =
        basis_.insertEchelonWithPivot(std::move(direction), pivot, statistics);
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
      basis_.canonicalize(statistics);
      is_identity_ = false;
      basis_.reduceInPlace(offset_, statistics);
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
    local_multiplier.emplace(dimension_, right.offset_, layout_);
    multiply_by_b = &*local_multiplier;
  }
  BitVector point = multiply_by_b->multiply(a, statistics);
  const bool point_is_identity = empty_ && identityPoint(point);
  add_point(std::move(point), point_is_identity);
  const std::size_t left_rank = left.rank(), right_rank = right.rank();
  if (full() || (left_rank == 0 && right_rank == 0))
    return finish();
  for (const auto &u : left.basis_.rows()) {
    if (full())
      break;
    add(multiply_by_b->multiply(u, statistics));
  }
  if (full() || right_rank == 0)
    return finish();
  for (const auto &direction : right.basis_.rows()) {
    if (full())
      break;
    const RightMatrixMultiplier multiply_by_v(dimension_, direction, layout_);
    add(multiply_by_v.multiply(a, statistics));
    for (const auto &u : left.basis_.rows()) {
      if (full())
        break;
      add(multiply_by_v.multiply(u, statistics));
    }
  }
  return finish();
}
bool AffineSpace::joinProductDelta(const AffineSpace &left,
                                   const AffineSpace &right,
                                   const AffineDelta &input, bool input_is_left,
                                   AffineDelta *output,
                                   const RightMatrixMultiplier *prepared_right,
                                   AlgebraStatistics *statistics) {
  check(left.dimension_);
  check(right.dimension_);
  if (input.empty() || left.empty_ || right.empty_)
    return false;
  if (input.full)
    return output ? joinProductWithDelta(left, right, prepared_right, output,
                                         statistics)
                  : joinProduct(left, right, prepared_right, statistics);
  if (empty_)
    return output ? joinProductWithDelta(left, right, prepared_right, output,
                                         statistics)
                  : joinProduct(left, right, prepared_right, statistics);
  if (rank() == offset_.size())
    return false;
  bool changed = false;
  auto full = [&] { return rank() == offset_.size(); };
  auto add = [&](BitVector direction) {
    if (full())
      return;
    std::size_t pivot;
    const bool inserted =
        output ? basis_.insertWithPivot(std::move(direction), pivot, statistics)
               : basis_.insert(std::move(direction), statistics);
    if (inserted && output)
      output->pivots.push_back(pivot);
    changed = inserted || changed;
  };
  auto forEachInputDirection = [&](const AffineSpace &space,
                                   const auto &function) {
    const auto &pivots = space.basis_.pivots();
    const auto &rows = space.basis_.rows();
    for (std::size_t pivot : input.pivots) {
      const auto *const found =
          std::lower_bound(pivots.begin(), pivots.end(), pivot);
      if (found == pivots.end() || *found != pivot)
        throw std::logic_error("affine delta pivot is absent from input");
      function(rows[static_cast<std::size_t>(found - pivots.begin())]);
      if (full())
        break;
    }
  };
  if (input_is_left) {
    std::optional<RightMatrixMultiplier> local_multiplier;
    const RightMatrixMultiplier *multiply_by_offset = prepared_right;
    if (!multiply_by_offset) {
      local_multiplier.emplace(dimension_, right.offset_, layout_);
      multiply_by_offset = &*local_multiplier;
    }
    forEachInputDirection(left, [&](const BitVector &direction) {
      add(multiply_by_offset->multiply(direction, statistics));
    });
    for (const auto &right_direction : right.basis_.rows()) {
      if (full())
        break;
      const RightMatrixMultiplier multiplier(dimension_, right_direction,
                                             layout_);
      forEachInputDirection(left, [&](const BitVector &direction) {
        add(multiplier.multiply(direction, statistics));
      });
    }
  } else {
    forEachInputDirection(right, [&](const BitVector &direction) {
      const RightMatrixMultiplier multiply_by_direction(dimension_, direction,
                                                        layout_);
      add(multiply_by_direction.multiply(left.offset_, statistics));
      for (const auto &left_direction : left.basis_.rows()) {
        if (full())
          break;
        add(multiply_by_direction.multiply(left_direction, statistics));
      }
    });
  }
  if (changed) {
    is_identity_ = false;
    basis_.reduceInPlace(offset_, statistics);
  }
  return changed;
}
bool AffineSpace::joinTripleProductDelta(
    const AffineSpace &left, const AffineSpace &middle,
    const AffineSpace &right, const AffineDelta &input, bool input_is_middle,
    AffineDelta *output, AlgebraStatistics *statistics) {
  check(left.dimension_);
  check(middle.dimension_);
  check(right.dimension_);
  if (input.empty() || left.empty_ || middle.empty_ || right.empty_)
    return false;
  if (rank() == offset_.size())
    return false;
  if (input.full || empty_) {
    const AffineSpace continuation = middle.product(right, statistics);
    return output ? joinProductWithDelta(left, continuation, nullptr, output,
                                         statistics)
                  : joinProduct(left, continuation, nullptr, statistics);
  }
  bool changed = false;
  auto full = [&] { return rank() == offset_.size(); };
  auto add = [&](BitVector direction) {
    if (full())
      return;
    std::size_t pivot;
    const bool inserted =
        output ? basis_.insertWithPivot(std::move(direction), pivot, statistics)
               : basis_.insert(std::move(direction), statistics);
    if (inserted && output)
      output->pivots.push_back(pivot);
    changed = inserted || changed;
  };
  auto forEachInputDirection = [&](const AffineSpace &space,
                                   const auto &function) {
    const auto &pivots = space.basis_.pivots();
    const auto &rows = space.basis_.rows();
    for (std::size_t pivot : input.pivots) {
      const auto *const found =
          std::lower_bound(pivots.begin(), pivots.end(), pivot);
      if (found == pivots.end() || *found != pivot)
        throw std::logic_error("affine delta pivot is absent from input");
      function(rows[static_cast<std::size_t>(found - pivots.begin())]);
      if (full())
        break;
    }
  };
  if (input_is_middle) {
    const RightMatrixMultiplier multiply_by_right_offset(
        dimension_, right.offset_, layout_);
    forEachInputDirection(middle, [&](const BitVector &direction) {
      const RightMatrixMultiplier multiply_by_delta(dimension_, direction,
                                                    layout_);
      llvm::SmallVector<BitVector, 2> partials;
      partials.push_back(multiply_by_delta.multiply(left.offset_, statistics));
      for (const auto &left_direction : left.basis_.rows())
        partials.push_back(
            multiply_by_delta.multiply(left_direction, statistics));
      for (const auto &partial : partials)
        add(multiply_by_right_offset.multiply(partial, statistics));
      for (const auto &right_direction : right.basis_.rows()) {
        if (full())
          break;
        const RightMatrixMultiplier multiply_by_right_direction(
            dimension_, right_direction, layout_);
        for (const auto &partial : partials) {
          if (full())
            break;
          add(multiply_by_right_direction.multiply(partial, statistics));
        }
      }
    });
  } else {
    const AffineSpace prefix = left.product(middle, statistics);
    forEachInputDirection(right, [&](const BitVector &direction) {
      const RightMatrixMultiplier multiply_by_delta(dimension_, direction,
                                                    layout_);
      add(multiply_by_delta.multiply(prefix.offset_, statistics));
      for (const auto &prefix_direction : prefix.basis_.rows()) {
        if (full())
          break;
        add(multiply_by_delta.multiply(prefix_direction, statistics));
      }
    });
  }
  if (changed) {
    is_identity_ = false;
    basis_.reduceInPlace(offset_, statistics);
  }
  return changed;
}
AffineSpace AffineSpace::block(std::size_t offset,
                               std::size_t dimension) const {
  if (!dimension || offset > dimension_ || dimension > dimension_ - offset)
    throw std::out_of_range("affine block");
  auto extract = [&](const BitVector &entries) {
    BitVector bits(Matrix::coordinateCount(dimension));
    for (std::size_t row = 0; row < dimension; ++row)
      for (std::size_t column = 0; column < dimension; ++column) {
        const auto index =
            layout_ ? layout_->coordinate(offset + row, offset + column)
                    : (offset + row) * dimension_ + offset + column;
        if (index < entries.size() && entries.test(index))
          bits.set(row * dimension + column);
      }
    return bits;
  };
  AffineSpace result(dimension);
  if (!empty_) {
    result.addPoint(Matrix(dimension, extract(offset_)));
    for (const auto &row : basis_.rows())
      result.addDirection(extract(row));
  }
  return result;
}

bool AffineSpace::operator==(const AffineSpace &other) const {
  if (dimension_ != other.dimension_ || empty_ != other.empty_)
    return false;
  if (!empty_ && !compatibleLayout(other))
    return expanded() == other.expanded();
  return empty_ || (offset_ == other.offset_ && basis_ == other.basis_);
}
bool SeparationCertificate::verify(const AffineSpace &left,
                                   const AffineSpace &right) const {
  if (left.empty() || right.empty() || left_value == right_value ||
      left.dimension() != functional.dimension() ||
      right.dimension() != functional.dimension())
    return false;
  if (!left.compatibleLayout(right))
    return verify(left.expanded(), right.expanded());
  const auto f =
      left.layout_ ? left.layout_->project(functional) : functional.entries();
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
                                              const AffineSpace &right,
                                              AlgebraStatistics *statistics) {
  left.check(right.dimension_);
  if (statistics)
    ++statistics->intersection_tests;
  if (left.empty_ || right.empty_)
    return std::nullopt;
  if (!left.compatibleLayout(right))
    return separate(left.expanded(), right.expanded(), statistics);
  if (left.offset_ == right.offset_ || left.rank() == left.coordinates() ||
      right.rank() == right.coordinates()) {
    if (statistics)
      ++statistics->intersection_fast_paths;
    return std::nullopt;
  }
  auto residual = left.basis_.reduce(left.offset_ ^ right.offset_, statistics);
  if (residual.empty()) {
    if (statistics)
      ++statistics->intersection_fast_paths;
    return std::nullopt;
  }
  LinearBasis combined = left.basis_;
  for (const auto &row : right.basis_.rows())
    if (combined.insertEchelon(row, statistics)) {
      combined.reduceInPlace(residual, statistics);
      if (residual.empty())
        return std::nullopt;
    }
  const auto started = std::chrono::steady_clock::now();
  combined.canonicalize(statistics);
  residual = combined.reduce(left.offset_ ^ right.offset_, statistics);
  const auto free = residual.firstSet();
  BitVector functional(residual.size());
  functional.set(free);
  for (std::size_t i = 0; i < combined.rows().size(); ++i)
    functional.set(combined.pivots()[i], combined.rows()[i].test(free));
  const bool l = functional.dot(left.offset_),
             r = functional.dot(right.offset_);
  auto result = SeparationCertificate{left.decodeEntries(functional), l, r};
  if (statistics)
    statistics->certificate_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count();
  return result;
}
AffineSemiring::AffineSemiring(std::size_t dimension,
                               std::shared_ptr<const MatrixLayout> layout)
    : dimension_(dimension), layout_(std::move(layout)),
      identity_(Matrix::identity(dimension)), zero_(dimension, layout_),
      one_(AffineSpace::singleton(identity_, layout_)) {}
void AffineSemiring::check(const Weight &weight) const {
  if (weight.dimension() != dimension_ || !weight.compatibleLayout(zero_))
    throw std::invalid_argument("semiring dimension mismatch");
}
AffineSemiring::Weight AffineSemiring::lift(const Matrix &matrix) const {
  if (matrix.dimension() != dimension_)
    throw std::invalid_argument("event matrix dimension mismatch");
  return matrix == identity_ ? one_ : AffineSpace::singleton(matrix, layout_);
}
AffineSemiring::Weight AffineSemiring::combine(const Weight &left,
                                               const Weight &right) const {
  check(left);
  check(right);
  Weight result = left;
  result.joinWith(right, statistics_.get());
  return result;
}
bool AffineSemiring::combineWith(Weight &left, const Weight &right) const {
  check(left);
  check(right);
  return left.joinWith(right, statistics_.get());
}
bool AffineSemiring::combineWithDelta(Weight &left, const Weight &right,
                                      Delta *delta) const {
  check(left);
  check(right);
  return left.joinWithDelta(right, delta, statistics_.get());
}
AffineSemiring::Weight AffineSemiring::extend(const Weight &left,
                                              const Weight &right) const {
  check(left);
  check(right);
  return left.product(right, statistics_.get());
}
bool AffineSemiring::extendAndCombine(Weight &target, const Weight &left,
                                      const Weight &right) const {
  check(target);
  check(left);
  check(right);
  return target.joinProduct(left, right, nullptr, statistics_.get());
}
bool AffineSemiring::extendAndCombineDelta(Weight &target, const Weight &left,
                                           const Weight &right,
                                           Delta *delta) const {
  check(target);
  check(left);
  check(right);
  return target.joinProductWithDelta(left, right, nullptr, delta,
                                     statistics_.get());
}
bool AffineSemiring::extendDeltaAndCombine(Weight &target, const Weight &left,
                                           const Weight &right,
                                           const Delta &input,
                                           bool input_is_left,
                                           Delta *output) const {
  check(target);
  check(left);
  check(right);
  return target.joinProductDelta(left, right, input, input_is_left, output,
                                 nullptr, statistics_.get());
}
AffineSemiring::PreparedWeight
AffineSemiring::prepareWeight(const Weight &weight) const {
  check(weight);
  if (weight.empty() || weight.rank() != 0 || weight.isIdentity() ||
      dimension_ > 64)
    return nullptr;
  auto prepared = std::make_shared<RightMatrixMultiplier>(
      dimension_, weight.offset(), layout_);
  return prepared;
}
AffineSemiring::Weight
AffineSemiring::extendPrepared(const Weight &left, const Weight &right,
                               const PreparedWeight &prepared_right) const {
  check(left);
  check(right);
  if (!prepared_right)
    return left.product(right, statistics_.get());
  Weight result(dimension_, layout_);
  result.joinProduct(left, right, prepared_right.get(), statistics_.get());
  return result;
}
bool AffineSemiring::extendAndCombinePrepared(
    Weight &target, const Weight &left, const Weight &right,
    const PreparedWeight &prepared_right) const {
  check(target);
  check(left);
  check(right);
  return prepared_right
             ? target.joinProduct(left, right, prepared_right.get(),
                                  statistics_.get())
             : target.joinProduct(left, right, nullptr, statistics_.get());
}
bool AffineSemiring::extendAndCombinePreparedDelta(
    Weight &target, const Weight &left, const Weight &right,
    const PreparedWeight &prepared_right, Delta *delta) const {
  check(target);
  check(left);
  check(right);
  return prepared_right
             ? target.joinProductWithDelta(left, right, prepared_right.get(),
                                           delta, statistics_.get())
             : target.joinProductWithDelta(left, right, nullptr, delta,
                                           statistics_.get());
}
bool AffineSemiring::extendPreparedInputDeltaAndCombine(
    Weight &target, const Weight &left, const Weight &right,
    const PreparedWeight &prepared_right, const Delta &input,
    Delta *output) const {
  check(target);
  check(left);
  check(right);
  return target.joinProductDelta(left, right, input, true, output,
                                 prepared_right.get(), statistics_.get());
}
bool AffineSemiring::extendPushDeltaAndCombine(
    Weight &target, const Weight &rule, const Weight &first,
    const Weight &second, const Delta &input, bool input_is_first,
    Delta *output) const {
  check(target);
  check(rule);
  check(first);
  check(second);
  return target.joinTripleProductDelta(
      rule, first, second, input, input_is_first, output, statistics_.get());
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

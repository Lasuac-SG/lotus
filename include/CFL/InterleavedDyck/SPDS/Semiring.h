#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace lotus::cfl::interleaved_dyck::spds {

// extend(a,b) always means execution of a FOLLOWED BY execution of b.
struct BooleanSemiring {
  using Weight = bool;
  Weight zero() const { return false; }
  Weight one() const { return true; }
  Weight combine(Weight a, Weight b) const { return a || b; }
  Weight extend(Weight a, Weight b) const { return a && b; }
};

// A finite binary relation on typestate states. There is no 32/64-state cap.
class RelationWeight {
public:
  explicit RelationWeight(std::size_t states = 0) : states_(states) {
    if (states > std::numeric_limits<std::size_t>::max() - 63)
      throw std::length_error("typestate dimension overflow");
    stride_ = (states + 63) / 64;
    if (stride_ && states > std::numeric_limits<std::size_t>::max() / stride_)
      throw std::length_error("typestate matrix overflow");
    bits_.resize(states * stride_, 0);
  }
  std::size_t states() const { return states_; }
  bool contains(std::size_t from, std::size_t to) const {
    check(from, to);
    return (bits_[from * stride_ + to / 64] >> (to % 64)) & 1U;
  }
  void insert(std::size_t from, std::size_t to) {
    check(from, to);
    bits_[from * stride_ + to / 64] |= std::uint64_t{1} << (to % 64);
  }
  bool operator==(const RelationWeight &other) const {
    return states_ == other.states_ && bits_ == other.bits_;
  }
  bool operator!=(const RelationWeight &other) const { return !(*this == other); }
private:
  friend class RelationSemiring;
  void check(std::size_t from, std::size_t to) const {
    if (from >= states_ || to >= states_)
      throw std::out_of_range("typestate index");
  }
  std::size_t states_ = 0, stride_ = 0;
  std::vector<std::uint64_t> bits_;
};

// combine = union; extend = relational composition in program execution order.
// Finite height guarantees termination of weighted saturation, including cycles.
class RelationSemiring {
public:
  using Weight = RelationWeight;
  explicit RelationSemiring(std::size_t states = 1) : states_(states) {
    if (!states) throw std::invalid_argument("empty typestate domain");
    (void)zero(); // Check dimensional arithmetic immediately.
  }
  std::size_t states() const { return states_; }
  Weight zero() const { return Weight(states_); }
  Weight one() const {
    auto result = zero();
    for (std::size_t i = 0; i < states_; ++i) result.insert(i, i);
    return result;
  }
  Weight transition(std::size_t from, std::size_t to) const {
    auto result = zero();
    result.insert(from, to);
    return result;
  }
  Weight combine(const Weight &a, const Weight &b) const {
    check(a); check(b);
    auto result = a;
    for (std::size_t i = 0; i < result.bits_.size(); ++i)
      result.bits_[i] |= b.bits_[i];
    return result;
  }
  Weight extend(const Weight &a, const Weight &b) const {
    check(a); check(b);
    auto result = zero();
    for (std::size_t i = 0; i < states_; ++i)
      for (std::size_t j = 0; j < states_; ++j)
        if (a.contains(i, j))
          for (std::size_t k = 0; k < result.stride_; ++k)
            result.bits_[i * result.stride_ + k] |= b.bits_[j * b.stride_ + k];
    return result;
  }
private:
  void check(const Weight &weight) const {
    if (weight.states() != states_)
      throw std::invalid_argument("incompatible typestate dimensions");
  }
  std::size_t states_;
};

} // namespace lotus::cfl::interleaved_dyck::spds

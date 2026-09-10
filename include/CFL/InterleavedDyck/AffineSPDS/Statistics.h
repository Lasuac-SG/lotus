#pragma once

#include <cstdint>

namespace lotus::cfl::interleaved_dyck::affine {

// Owned by one query, never global or shared between independent queries.
struct AlgebraStatistics {
  std::uint64_t matrix_products = 0;
  std::uint64_t basis_reductions = 0;
  std::uint64_t basis_insertions = 0;
  std::uint64_t cow_detaches = 0;
  std::uint64_t intersection_tests = 0;
  std::uint64_t intersection_fast_paths = 0;
  std::uint64_t intersection_us = 0;
  std::uint64_t certificate_us = 0;
};

} // namespace lotus::cfl::interleaved_dyck::affine

#include "InterleavedDyckAffineSPDSTestSupport.h"
#include <gtest/gtest.h>
TEST(InterleavedDyckAffineSPDS, SharedSuites) {
  for (const auto &test : lotus::cfl::interleaved_dyck::affine::test::tests()) {
    SCOPED_TRACE(test.first);
    EXPECT_NO_THROW(test.second());
  }
}

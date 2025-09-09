#include "simcore/backpressure.hpp"
#include <gtest/gtest.h>
#include <thread>

using namespace simcore;

TEST(TokenBucket, RefillsOverTime) {
  TokenBucket tb(2, 10.0); // 2 tokens max, 10 tokens per second
  EXPECT_TRUE(tb.try_acquire());
  EXPECT_TRUE(tb.try_acquire());
  EXPECT_FALSE(tb.try_acquire());
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_TRUE(tb.try_acquire());
}

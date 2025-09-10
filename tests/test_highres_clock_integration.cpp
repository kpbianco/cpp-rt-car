#include <gtest/gtest.h>
#include "simcore/highres_clock.hpp"

TEST(HighResClockIntegration, MonotonicSequence) {
    HighResClock::init();
    uint64_t last = HighResClock::now();
    for (int i = 0; i < 10000; ++i) {
        uint64_t cur = HighResClock::now();
        EXPECT_GT(cur, last);
        last = cur;
    }
}

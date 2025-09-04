#include <gtest/gtest.h>
#include "simcore/rate_governor.hpp"

TEST(RateGovernor, AdversarialBurstRecovers) {
    RateGovernor rg(16.0); // 16ms frame
    // warmup stable frames
    for(int i=0;i<5;i++) {
        double s = rg.update(10.0);
        EXPECT_NEAR(s, 1.0, 0.1);
    }
    // adversarial burst
    double s = rg.update(40.0);
    EXPECT_LT(s, 1.0);
    // subsequent frames should recover
    for(int i=0;i<40;i++) {
        s = rg.update(10.0);
    }
    EXPECT_GT(s, 0.95);
}


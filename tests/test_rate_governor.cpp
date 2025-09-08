#include <gtest/gtest.h>
#include "simcore/rate_governor.hpp"
#include <vector>

TEST(RateGovernor, AdversarialBurstTriggersLadderAndRecovers) {
    std::vector<int> events;
    RateGovernor rg(16.0, 0.1, 0.01, 0.5, 1.0, 0.05,
                    [&](int rung){ events.push_back(rung); });
    // warmup stable frames
    for(int i=0;i<5;i++) {
        double s = rg.update(10.0);
        EXPECT_NEAR(s, 1.0, 0.1);
    }
    // adversarial burst
    double s = rg.update(40.0);
    EXPECT_LT(s, 1.0);
    EXPECT_EQ(rg.rung(), 3);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0], 1);
    EXPECT_EQ(events[1], 2);
    EXPECT_EQ(events[2], 3);
    // subsequent frames should recover
    for(int i=0;i<40;i++) {
        s = rg.update(10.0);
    }
    EXPECT_GT(s, 0.95);
}


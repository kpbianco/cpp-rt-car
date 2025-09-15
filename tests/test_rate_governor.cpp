#include <gtest/gtest.h>
#include "simcore/rate_governor.hpp"
#include <vector>

TEST(RateGovernor, AdversarialBurstTriggersLadderAndRecovers) {
    std::vector<int> events;
    RateGovernor rg(16.0, 0.1, 0.01, 0.5, 1.0, 0.05,
                    [&](int rung){ events.push_back(rung); });
    // warmup stable frames
    RateGovernor::UpdateResult res;
    for(int i=0;i<5;i++) {
        res = rg.update(10.0);
        EXPECT_NEAR(res.scale, 1.0, 0.1);
    }
    // adversarial burst
    res = rg.update(40.0);
    EXPECT_LT(res.scale, 1.0);
    EXPECT_EQ(rg.rung(), 3);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0], 1);
    EXPECT_EQ(events[1], 2);
    EXPECT_EQ(events[2], 3);
    // subsequent frames should recover
    for(int i=0;i<40;i++) {
        res = rg.update(10.0);
    }
    EXPECT_GT(res.scale, 0.95);
}


#include <gtest/gtest.h>
#include "simcore/rate_governor.hpp"
#include <vector>

TEST(RateGovernor, AdversarialBurstTriggersLadderAndRecovers) {
    std::vector<int> events;
    RateGovernor rg(0.1, 0.01, 0.5, 1.0);
    rg.setCallback([&](int rung) { events.push_back(rung); });
    // warmup stable frames
    for(int i=0;i<5;i++) {
        double s = rg.update(10.0, 16.0, 0.9, 0.05);
        EXPECT_NEAR(s, 1.0, 0.1);
    }
    // adversarial burst
    double s = rg.update(40.0, 16.0, 0.9, 0.05);
    EXPECT_LT(s, 1.0);
    EXPECT_EQ(rg.rung(), 3);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0], 1);
    EXPECT_EQ(events[1], 2);
    EXPECT_EQ(events[2], 3);
    EXPECT_EQ(rg.rungCount(1), 1u);
    EXPECT_EQ(rg.rungCount(2), 1u);
    EXPECT_EQ(rg.rungCount(3), 1u);
    // subsequent frames should recover
    for(int i=0;i<40;i++) {
        s = rg.update(10.0, 16.0, 0.9, 0.05);
    }
    EXPECT_GT(s, 0.95);
}

TEST(RateGovernor, CountsEachBoundedRungOnce) {
    std::vector<int> events;
    RateGovernor rg;
    rg.setCallback([&](int rung) { events.push_back(rung); });

    rg.update(6.5, 10.0, 0.5, 0.1);
    rg.update(7.5, 10.0, 0.5, 0.1);
    rg.update(8.5, 10.0, 0.5, 0.1);
    rg.update(9.0, 10.0, 0.5, 0.1);

    EXPECT_EQ(rg.rung(), 3);
    EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(rg.rungCount(0), 0u);
    EXPECT_EQ(rg.rungCount(1), 1u);
    EXPECT_EQ(rg.rungCount(2), 1u);
    EXPECT_EQ(rg.rungCount(3), 1u);
    EXPECT_EQ(rg.rungCount(4), 0u);
}

#include <gtest/gtest.h>
#include <thread>
#include "simcore/highres_clock.hpp"

TEST(HighResClock, MonotonicMockedTSC) {
    HighResClock::init([](){ static uint64_t c=0; return c+=100; });
    uint64_t a = HighResClock::now();
    uint64_t b = HighResClock::now();
    EXPECT_LT(a, b);
    uint64_t c = HighResClock::now();
    EXPECT_LT(b, c);
}

TEST(HighResClock, DriftDisablesTSC) {
    static uint64_t c = 0;
    static bool slow = false;
    auto reader = [](){ if (slow) c += 10; else c += 1000; return c; };
    HighResClock::init(reader);
    ASSERT_TRUE(HighResClock::using_tsc());
    (void)HighResClock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)HighResClock::now();
    slow = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (int i=0;i<5000;i++) HighResClock::now();
    EXPECT_FALSE(HighResClock::using_tsc());
}

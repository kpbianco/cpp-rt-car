#include <gtest/gtest.h>
#include <rt/watchdog.hpp>
#include <atomic>
#include <thread>

TEST(RtWatchdog, LimpModeOnStall) {
    std::atomic<int> limps{0};
    rt::Watchdog wd(std::chrono::milliseconds(50),
                    std::chrono::milliseconds(200),
                    [&]{ ++limps; });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_TRUE(wd.limp());
    EXPECT_EQ(limps.load(), 1);
    wd.touch();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(limps.load(), 1); // within relaxed budget
}


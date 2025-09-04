#include <gtest/gtest.h>
#include <simcore/watchdog.hpp>
#include <atomic>
#include <thread>

TEST(Watchdog, TriggersCallback)
{
    std::atomic<bool> fired{false};
    {
        Watchdog wd(std::chrono::milliseconds(50), [&]{ fired = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    EXPECT_TRUE(fired.load());
}

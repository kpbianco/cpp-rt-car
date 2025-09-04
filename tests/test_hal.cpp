#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "simcore/hal.hpp"

using namespace std::chrono_literals;

TEST(HAL, ThreadAndFence) {
    std::atomic<int> value{0};
    simcore::hal::Fence fence = simcore::hal::fence_create();
    simcore::hal::Thread t = simcore::hal::create_thread([&]() {
        value.store(1, std::memory_order_release);
        simcore::hal::fence_signal(fence);
    });
    EXPECT_TRUE(simcore::hal::fence_wait(fence, 1000ms));
    simcore::hal::join(t);
    EXPECT_EQ(value.load(std::memory_order_acquire), 1);
}

TEST(HAL, AllocFree) {
    void* p = simcore::hal::alloc(128);
    ASSERT_NE(p, nullptr);
    simcore::hal::free(p);
}

TEST(HAL, TimerElapsed) {
    auto start = simcore::hal::now();
    std::this_thread::sleep_for(10ms);
    auto end = simcore::hal::now();
    auto d = simcore::hal::elapsed(start, end);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(d).count(), 10);
}


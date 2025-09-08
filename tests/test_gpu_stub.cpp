#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <gpu_stub.hpp>
#include <rt/fiber_pool.hpp>
#include <simcore/hal.hpp>
#include <atomic>
#include <thread>

using namespace std::chrono_literals;

static rt::Task wait_task(rt::FiberPool& pool, simcore::hal::Fence fence, std::atomic<bool>& done) {
    co_await rt::FiberPool::FenceAwaiter{fence, pool};
    done.store(true, std::memory_order_relaxed);
}

static rt::Task cpu_task(std::chrono::milliseconds dur, std::atomic<bool>& done) {
    auto start = simcore::hal::now();
    while (simcore::hal::elapsed(start, simcore::hal::now()) < dur) {
        // busy work
    }
    done.store(true, std::memory_order_relaxed);
    co_return;
}

TEST(GPUStub, Overlap) {
    SimCore::Settings s; s.threads = 1; SimCore sim(s);
    auto& pool = sim.fiberPool();
    constexpr auto gpu_time = 50ms;
    auto fence = simcore::hal::gpu::submit([]() { std::this_thread::sleep_for(gpu_time); });

    std::atomic<bool> fence_done{false};
    std::atomic<bool> cpu_done{false};

    pool.spawn(wait_task(pool, fence, fence_done));
    pool.spawn(cpu_task(gpu_time, cpu_done));

    auto start = simcore::hal::now();
    pool.drain();
    auto total = simcore::hal::elapsed(start, simcore::hal::now());

    EXPECT_TRUE(fence_done.load());
    EXPECT_TRUE(cpu_done.load());

    auto expected = gpu_time + gpu_time;
    auto overlap = expected - total;
    double pct = std::chrono::duration<double>(overlap).count() /
                 std::chrono::duration<double>(gpu_time).count();
    EXPECT_GT(pct, 0.5);
}


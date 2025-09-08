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
    done.store(true, std::memory_order_release);
}

static rt::Task cpu_task(std::chrono::milliseconds dur,
                         std::atomic<bool>& done,
                         std::atomic<simcore::hal::Duration::rep>& elapsed_ns) {
    auto start = simcore::hal::now();
    while (simcore::hal::elapsed(start, simcore::hal::now()) < dur) {
        // busy work
    }
    elapsed_ns.store(simcore::hal::elapsed(start, simcore::hal::now()).count(),
                     std::memory_order_release);
    done.store(true, std::memory_order_release);
    co_return;
}

TEST(GPUStub, Overlap) {
    SimCore::Settings s; s.threads = 1; SimCore sim(s);
    auto& pool = sim.fiberPool();
    constexpr auto gpu_time = 50ms;
    std::atomic<simcore::hal::Duration::rep> gpu_ns{0};
    auto fence = simcore::hal::gpu::submit([&]() {
        auto start = simcore::hal::now();
        std::this_thread::sleep_for(gpu_time);
        gpu_ns.store(simcore::hal::elapsed(start, simcore::hal::now()).count(),
                     std::memory_order_release);
    });

    std::atomic<bool> fence_done{false};
    std::atomic<bool> cpu_done{false};
    std::atomic<simcore::hal::Duration::rep> cpu_ns{0};

    pool.spawn(wait_task(pool, fence, fence_done));
    pool.spawn(cpu_task(gpu_time, cpu_done, cpu_ns));

    auto start = simcore::hal::now();
    pool.drain();
    auto total = simcore::hal::elapsed(start, simcore::hal::now());

    EXPECT_TRUE(fence_done.load(std::memory_order_acquire));
    EXPECT_TRUE(cpu_done.load(std::memory_order_acquire));

    simcore::hal::Duration gpu_actual{gpu_ns.load(std::memory_order_acquire)};
    simcore::hal::Duration cpu_actual{cpu_ns.load(std::memory_order_acquire)};
    auto expected = gpu_actual + cpu_actual;
    auto overlap = expected - total;
    // On some platforms timer resolution or scheduling can reduce measured
    // overlap; just ensure there was any overlap at all.
    EXPECT_GT(overlap.count(), 0);
}


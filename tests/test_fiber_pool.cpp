#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <rt/fiber_pool.hpp>
#include <simcore/hal.hpp>
#include <vector>
#include <atomic>

using namespace std::chrono_literals;

static rt::Task fence_task(rt::FiberPool& pool, simcore::hal::Fence& f,
                           std::atomic<std::size_t>& counter) {
    co_await rt::FiberPool::FenceAwaiter{f, pool};
    counter.fetch_add(1, std::memory_order_relaxed);
}

TEST(FiberPool, FenceStormSimCore) {
    for (int threads : {1, 4}) {
        SimCore::Settings s;
        s.threads = static_cast<std::size_t>(threads);
        SimCore sim(s);
        auto& pool = sim.fiberPool();
        std::atomic<std::size_t> done{0};
        const std::size_t N = 64;
        std::vector<simcore::hal::Fence> fences(N);
        for (std::size_t i = 0; i < N; ++i) {
            pool.spawn(fence_task(pool, fences[i], done));
        }
        for (auto& f : fences) {
            std::thread([&f]() {
                std::this_thread::sleep_for(1ms);
                simcore::hal::fence_signal(f);
            }).detach();
        }
        pool.drain();
        EXPECT_EQ(done.load(std::memory_order_relaxed), N);
    }
}


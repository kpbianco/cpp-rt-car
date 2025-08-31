#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include "frame_arena.hpp" // shim

TEST(RtPool, DeterministicBindingAndUse) {
    const std::size_t N = 4;
    FrameArenaPool pool(N, 1u<<20, 64);

    std::vector<std::thread> workers;
    std::atomic<int> ok{0};

    for (std::size_t i = 0; i < N; ++i) {
        workers.emplace_back([&, i](){
            pool.bindCurrentThread(i);
            auto& arena = pool.tls();
            auto before = arena.used();
            (void)arena.allocate(128, 64);
            auto after = arena.used();
            if (after > before) ok.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : workers) t.join();

    EXPECT_EQ(ok.load(std::memory_order_relaxed), static_cast<int>(N));

    pool.beginFrame();
    for (std::size_t i = 0; i < N; ++i) {
        EXPECT_EQ(pool.arena(i).used(), 0u);
    }
}

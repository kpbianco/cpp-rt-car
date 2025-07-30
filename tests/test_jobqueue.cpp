#include <gtest/gtest.h>
#include "worker_pool.hpp"
#include <atomic>

TEST(JobQueue, ExecutesEveryJob)
{
    constexpr int N = 10000;
    WorkerPool pool(4);
    std::atomic<int> counter{0};

    for (int i = 0; i < N; ++i)
        pool.enqueue({[&counter]{ counter.fetch_add(1,std::memory_order_relaxed); }});

    // spin‑wait until all executed
    while (counter.load(std::memory_order_acquire) != N)
        std::this_thread::yield();

    EXPECT_EQ(counter.load(), N);
}

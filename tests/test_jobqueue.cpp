#include <gtest/gtest.h>
#include <atomic>
#include <simcore/worker_pool.hpp>

TEST(JobQueue, ExecutesEveryJob) {
    // 4 workers, 2048-capacity queue (power-of-two)
    WorkerPool pool(4, 2048);

    std::atomic<int> counter{0};
    constexpr int N = 20000;

    for (int i = 0; i < N; ++i) {
        pool.enqueue([&counter]{
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Wait until all jobs finished, then stop pool (join threads)
    pool.drain();
    pool.stop();

    EXPECT_EQ(counter.load(), N);
}

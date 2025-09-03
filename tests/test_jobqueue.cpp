#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <simcore/worker_pool.hpp>
#include <thread>

TEST(JobQueue, ExecutesEveryJob) {
  // 4 workers, 2048-capacity queue (power-of-two)
  WorkerPool pool(4, 2048);

  std::atomic<int> counter{0};
  constexpr int N = 20000;

  for (int i = 0; i < N; ++i) {
    pool.enqueue(
        [&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  }

  // Wait until all jobs finished, then stop pool (join threads)
  pool.drain();
  pool.stop();

  EXPECT_EQ(counter.load(), N);
}

TEST(JobQueue, BackpressureCapsOutstanding) {
  // Small threshold to exercise guard
  WorkerPool pool(/*threads*/ 3, /*queue size*/ 64, /*verbose*/ false,
                  /*maxOutstanding*/ 4);

  std::atomic<std::size_t> peak{0};

  // Job that enqueues many sub-jobs mid-frame
  pool.enqueue([&] {
    for (int i = 0; i < 20; ++i) {
      pool.enqueue([&] {
        // Update peak outstanding seen
        auto cur = pool.outstanding();
        std::size_t prev = peak.load(std::memory_order_relaxed);
        while (cur > prev && !peak.compare_exchange_weak(
                                 prev, cur, std::memory_order_relaxed)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      });
      // Track after each enqueue
      auto cur = pool.outstanding();
      std::size_t prev = peak.load(std::memory_order_relaxed);
      while (cur > prev && !peak.compare_exchange_weak(
                               prev, cur, std::memory_order_relaxed)) {
      }
    }
  });

  pool.drain();
  pool.stop();

  EXPECT_LE(peak.load(), 4u);
}

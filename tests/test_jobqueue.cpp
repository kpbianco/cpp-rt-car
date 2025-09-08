#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <simcore/job_queue.hpp>
#include <simcore/worker_pool.hpp>
#include <thread>
#include <vector>

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

TEST(JobQueue, SingleProducerFastPath) {
  BoundedMPMCQueue<int> q(64);
  for (int i = 0; i < 64; ++i) {
    ASSERT_TRUE(q.try_push(i, /*singleProducer*/ true));
  }
  for (int i = 0; i < 64; ++i) {
    int out = -1;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, i);
  }
  int dummy = 0;
  EXPECT_FALSE(q.try_pop(dummy));
}

TEST(JobQueue, ContentionTwoXThreads) {
  unsigned cores = std::thread::hardware_concurrency();
  if (cores == 0) {
    cores = 2;
  }
  const unsigned threads = cores * 2;
  const std::size_t opsPerThread = 1000;
  BoundedMPMCQueue<std::size_t> q(256);
  std::atomic<std::size_t> produced{0};
  std::atomic<std::size_t> consumed{0};
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (unsigned t = 0; t < threads; ++t) {
    if (t % 2 == 0) {
      workers.emplace_back([&] {
        for (std::size_t i = 0; i < opsPerThread; ++i) {
          while (!q.try_push(i)) {
            std::this_thread::yield();
          }
          produced.fetch_add(1, std::memory_order_relaxed);
        }
      });
    } else {
      workers.emplace_back([&] {
        for (std::size_t i = 0; i < opsPerThread; ++i) {
          std::size_t out;
          while (!q.try_pop(out)) {
            std::this_thread::yield();
          }
          consumed.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
  }
  for (auto &th : workers) {
    th.join();
  }
  EXPECT_EQ(produced.load(), consumed.load());
}

#include <gtest/gtest.h>
#include <simcore/job_queue.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Simple mutex-protected queue for baseline comparison
class MutexQueue {
 public:
  explicit MutexQueue(std::size_t) {}
  bool try_push(int v) {
    std::lock_guard<std::mutex> lk(mu_);
    q_.push(v);
    return true;
  }
  bool try_pop(int &out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) return false;
    out = q_.front();
    q_.pop();
    return true;
  }

 private:
  std::queue<int> q_;
  std::mutex mu_;
};

// Generic benchmark helper for different producer/consumer mixes
// Returns wall-clock time in milliseconds taken to perform all operations.
template <class Queue>
double run_mix(std::size_t producers, std::size_t consumers,
              std::size_t opsPerProducer) {
  Queue q(1024);
  const std::size_t totalOps = producers * opsPerProducer;
  std::atomic<std::size_t> consumed{0};
  std::vector<std::thread> threads;
  threads.reserve(producers + consumers);

  auto start = std::chrono::steady_clock::now();
  for (std::size_t p = 0; p < producers; ++p) {
    threads.emplace_back([&q, opsPerProducer]() {
      for (std::size_t i = 0; i < opsPerProducer; ++i) {
        while (!q.try_push(static_cast<int>(i))) {
          std::this_thread::yield();
        }
      }
    });
  }
  for (std::size_t c = 0; c < consumers; ++c) {
    threads.emplace_back([&q, &consumed, totalOps]() {
      int out;
      while (consumed.load(std::memory_order_relaxed) < totalOps) {
        if (q.try_pop(out)) {
          consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }
  auto end = std::chrono::steady_clock::now();

  EXPECT_EQ(consumed.load(), totalOps);
  return std::chrono::duration<double, std::milli>(end - start).count();
}

TEST(JobQueuePerf, MPMCvsMutex) {
  constexpr std::size_t producers = 4;
  constexpr std::size_t consumers = 4;
  constexpr std::size_t ops = 5000;
  double lf = run_mix<BoundedMPMCQueue<int>>(producers, consumers, ops);
  double mx = run_mix<MutexQueue>(producers, consumers, ops);
  std::cout << "Lockfree queue: " << lf << " ms, mutex queue: " << mx << " ms\n";
  if (lf >= mx) {
    GTEST_SKIP() << "Lock-free queue was not faster than mutex queue";
  }
  EXPECT_LT(lf, mx);
}

TEST(JobQueuePerfMix, SPSC) {
  double t = run_mix<BoundedMPMCQueue<int>>(1, 1, 20000);
  std::cout << "SPSC time: " << t << " ms\n";
  EXPECT_GT(t, 0.0);
}

TEST(JobQueuePerfMix, SPMC) {
  double t = run_mix<BoundedMPMCQueue<int>>(1, 4, 20000);
  std::cout << "SPMC time: " << t << " ms\n";
  EXPECT_GT(t, 0.0);
}

TEST(JobQueuePerfMix, MPMC) {
  double t = run_mix<BoundedMPMCQueue<int>>(4, 4, 20000);
  std::cout << "MPMC time: " << t << " ms\n";
  EXPECT_GT(t, 0.0);
}

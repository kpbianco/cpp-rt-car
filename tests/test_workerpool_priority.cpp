#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <simcore/worker_pool.hpp>

using namespace std::chrono_literals;

TEST(WorkerPoolSched, HighPriorityRunsFirst) {
  WorkerPool pool(1);
  std::vector<int> order;
  std::mutex m;
  std::promise<void> gate;
  auto fut = gate.get_future();
  // Block worker until both tasks enqueued so priority ordering is deterministic
  pool.enqueue([&] { fut.wait(); });
  pool.enqueue(
      [&] {
        std::lock_guard<std::mutex> lk(m);
        order.push_back(1);
      },
      WorkerPool::Priority::Low);
  pool.enqueue(
      [&] {
        std::lock_guard<std::mutex> lk(m);
        order.push_back(2);
      },
      WorkerPool::Priority::High);
  gate.set_value();
  pool.drain();
  pool.stop();
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], 2); // high priority ran first
}

TEST(WorkerPoolSched, WorkStealing) {
  WorkerPool pool(4);
  std::set<std::thread::id> threads;
  std::mutex m;
  for (int i = 0; i < 50; ++i) {
    pool.enqueueOn(0, [&] {
      std::lock_guard<std::mutex> lk(m);
      threads.insert(std::this_thread::get_id());
    });
  }
  pool.drain();
  pool.stop();
  EXPECT_GT(threads.size(), 1u); // saw work on multiple threads
}

TEST(WorkerPoolSched, TailLatencyUnderSkew) {
  WorkerPool pool(1);
  std::atomic<bool> highDone{false};
  pool.enqueue([] { std::this_thread::sleep_for(100ms); },
               WorkerPool::Priority::Low);
  std::this_thread::sleep_for(10ms); // ensure low job starts
  auto start = std::chrono::steady_clock::now();
  pool.enqueue([&] { highDone.store(true, std::memory_order_release); },
               WorkerPool::Priority::High);
  while (!highDone.load(std::memory_order_acquire))
    std::this_thread::yield();
  auto elapsed = std::chrono::steady_clock::now() - start;
  pool.drain();
  pool.stop();
  EXPECT_LT(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      50);
}

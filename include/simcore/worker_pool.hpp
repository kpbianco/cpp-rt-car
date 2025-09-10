#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

#include "job_queue.hpp"
#include <rt/numerics.hpp>

// Simple worker pool backed by a bounded MPMC queue.  All work is routed
// through a single scheduling surface which allows the queue depth and
// steal metrics to be observed.  High priority work is serviced before
// normal work which runs before low priority jobs.
class WorkerPool {
public:
  enum class Priority { High, Normal, Low };
  enum class Category { CPU, GPU, IO };
  using JobFn = std::function<void()>;

  explicit WorkerPool(std::size_t numThreads,
                      std::size_t queueSizePow2 = 1024)
      : highQ_(queueSizePow2),
        normalQ_(queueSizePow2),
        lowQ_(queueSizePow2) {
    stopping_.store(false, std::memory_order_relaxed);
    outstanding_.store(0, std::memory_order_relaxed);
    maxDepth_.store(0, std::memory_order_relaxed);
    steals_.store(0, std::memory_order_relaxed);
    threads_.reserve(numThreads);
    for (std::size_t i = 0; i < numThreads; ++i) {
      threads_.emplace_back([this] { this->workerLoop(); });
    }
  }

  ~WorkerPool() { stop(); }

  // Submit work to the pool.  Blocks if the queue is full.
  void submit(JobFn fn, Priority pr = Priority::Normal,
              Category = Category::CPU) {
    auto &q = queueFor(pr);
    while (!q.try_push(Job{std::move(fn)})) {
      std::this_thread::yield();
    }
    auto depth = approx_queue_size();
    auto prev = maxDepth_.load(std::memory_order_relaxed);
    while (depth > prev &&
           !maxDepth_.compare_exchange_weak(prev, depth,
                                            std::memory_order_relaxed)) {
    }
    outstanding_.fetch_add(1, std::memory_order_acq_rel);
  }

  // Compatibility wrappers for older code.
  void enqueue(JobFn fn, Priority pr = Priority::Normal,
               Category cat = Category::CPU) {
    submit(std::move(fn), pr, cat);
  }
  void enqueueOn(std::size_t, JobFn fn, Priority pr = Priority::Normal,
                 Category cat = Category::CPU) {
    submit(std::move(fn), pr, cat);
  }

  // Wait for all outstanding jobs to complete.
  void drain() {
    using namespace std::chrono_literals;
    while (outstanding_.load(std::memory_order_acquire) > 0) {
      std::this_thread::sleep_for(50us);
    }
  }

  // Stop workers and join threads.
  void stop() {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
      // already stopping
    }
    drain();
    for (auto &t : threads_) {
      if (t.joinable())
        t.join();
    }
    threads_.clear();
  }

  std::size_t approx_queue_size() const {
    return highQ_.size() + normalQ_.size() + lowQ_.size();
  }

  std::size_t max_queue_depth() const {
    return maxDepth_.load(std::memory_order_relaxed);
  }

  std::size_t steals() const {
    return steals_.load(std::memory_order_relaxed);
  }

  std::size_t thread_count() const { return threads_.size(); }

private:
  struct Job { JobFn fn; };

  BoundedMPMCQueue<Job> &queueFor(Priority pr) {
    switch (pr) {
    case Priority::High:
      return highQ_;
    case Priority::Low:
      return lowQ_;
    default:
      return normalQ_;
    }
  }

  bool popJob(Job &out) {
    return highQ_.try_pop(out) || normalQ_.try_pop(out) || lowQ_.try_pop(out);
  }

  void workerLoop() {
    rt::init_fp_env();
    for (;;) {
      Job job;
      if (popJob(job)) {
        steals_.fetch_add(1, std::memory_order_relaxed);
        if (job.fn)
          job.fn();
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        continue;
      }
      if (stopping_.load(std::memory_order_acquire))
        break;
      std::this_thread::yield();
    }
  }

  BoundedMPMCQueue<Job> highQ_;
  BoundedMPMCQueue<Job> normalQ_;
  BoundedMPMCQueue<Job> lowQ_;

  std::vector<std::thread> threads_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::size_t> outstanding_{0};
  std::atomic<std::size_t> maxDepth_{0};
  std::atomic<std::size_t> steals_{0};
};


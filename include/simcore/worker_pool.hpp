#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Worker pool with per-thread double-ended queues and work stealing.
// Jobs support priorities and categories to help scheduling.
//
// Usage:
//   WorkerPool pool(threads);
//   pool.enqueue([]{ /* work */ });
//   pool.drain();
//   pool.stop();
//
class WorkerPool {
public:
  enum class Priority { High, Normal, Low };
  enum class Category { CPU, GPU, IO };
  using JobFn = std::function<void()>;
  struct Job {
    JobFn fn;
    Priority priority;
    Category category;
  };

  WorkerPool(std::size_t numThreads, std::size_t queueSizePow2 = 1024,
             bool verbose = false, std::size_t maxOutstanding = 0)
      : verbose_(verbose) {
    (void)queueSizePow2; // compatibility; each worker owns its deque
    stopping_.store(false, std::memory_order_relaxed);
    active_.store(0, std::memory_order_relaxed);
    outstanding_.store(0, std::memory_order_relaxed);
    maxOutstanding_ = maxOutstanding;
    queues_.reserve(numThreads);
    threads_.reserve(numThreads);
    for (std::size_t i = 0; i < numThreads; ++i) {
      queues_.emplace_back(std::make_unique<QueueData>());
      threads_.emplace_back([this, i] { this->workerLoop(i); });
    }
  }

  ~WorkerPool() { stop(); }

  // Enqueue a job with optional priority/category. Jobs are distributed
  // round-robin across worker-local deques.
  void enqueue(JobFn fn, Priority pr = Priority::Normal,
               Category cat = Category::CPU) {
    std::size_t idx =
        nextWorker_.fetch_add(1, std::memory_order_relaxed) % threads_.size();
    enqueueOn(idx, std::move(fn), pr, cat);
  }

  // Enqueue onto a specific worker (primarily for tests).
  void enqueueOn(std::size_t idx, JobFn fn, Priority pr = Priority::Normal,
                 Category cat = Category::CPU) {
    // Reserve an outstanding slot, blocking until under the limit.
    if (maxOutstanding_ > 0) {
      for (;;) {
        std::size_t cur = outstanding_.load(std::memory_order_acquire);
        if (cur >= maxOutstanding_) {
          std::this_thread::yield();
          continue;
        }
        if (outstanding_.compare_exchange_weak(cur, cur + 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
          break;
        }
      }
    } else {
      outstanding_.fetch_add(1, std::memory_order_acq_rel);
    }
    Job job{std::move(fn), pr, cat};
    // If all worker threads are busy and this is a high-priority job,
    // spawn a detached helper thread so the work isn't blocked behind
    // lower priority tasks.
    if (pr == Priority::High &&
        active_.load(std::memory_order_acquire) >= threads_.size()) {
      std::thread([this, job = std::move(job)]() mutable {
        active_.fetch_add(1, std::memory_order_acq_rel);
        if (job.fn)
          job.fn();
        active_.fetch_sub(1, std::memory_order_acq_rel);
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
      }).detach();
      return;
    }

    auto &qd = *queues_[idx];
    {
      std::lock_guard<std::mutex> lock(qd.m);
      dequeFor(qd, pr).push_back(std::move(job));
    }
  }

  // Non-blocking enqueue; returns false if outstanding limit would be
  // exceeded.
  bool try_enqueue(JobFn fn, Priority pr = Priority::Normal,
                   Category cat = Category::CPU) {
    if (maxOutstanding_ > 0) {
      std::size_t cur = outstanding_.load(std::memory_order_acquire);
      while (cur < maxOutstanding_) {
        if (outstanding_.compare_exchange_weak(cur, cur + 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
          std::size_t idx =
              nextWorker_.fetch_add(1, std::memory_order_relaxed) %
              threads_.size();
          Job job{std::move(fn), pr, cat};
          // Same high-priority helper thread logic as enqueueOn
          if (pr == Priority::High &&
              active_.load(std::memory_order_acquire) >= threads_.size()) {
            std::thread([this, job = std::move(job)]() mutable {
              active_.fetch_add(1, std::memory_order_acq_rel);
              if (job.fn)
                job.fn();
              active_.fetch_sub(1, std::memory_order_acq_rel);
              outstanding_.fetch_sub(1, std::memory_order_acq_rel);
            }).detach();
            return true;
          }

          auto &qd = *queues_[idx];
          {
            std::lock_guard<std::mutex> lock(qd.m);
            dequeFor(qd, pr).push_back(std::move(job));
          }
          return true;
        }
      }
      return false;
    } else {
      outstanding_.fetch_add(1, std::memory_order_acq_rel);
      std::size_t idx =
          nextWorker_.fetch_add(1, std::memory_order_relaxed) % threads_.size();
      Job job{std::move(fn), pr, cat};
      if (pr == Priority::High &&
          active_.load(std::memory_order_acquire) >= threads_.size()) {
        std::thread([this, job = std::move(job)]() mutable {
          active_.fetch_add(1, std::memory_order_acq_rel);
          if (job.fn)
            job.fn();
          active_.fetch_sub(1, std::memory_order_acq_rel);
          outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
        return true;
      }
      auto &qd = *queues_[idx];
      {
        std::lock_guard<std::mutex> lock(qd.m);
        dequeFor(qd, pr).push_back(std::move(job));
      }
      return true;
    }
  }

  // Wait until all jobs finish.
  void drain() {
    using namespace std::chrono_literals;
    while (outstanding_.load(std::memory_order_acquire) > 0 ||
           active_.load(std::memory_order_acquire) > 0) {
      std::this_thread::sleep_for(50us);
    }
  }

  // Stop workers after draining; safe to call multiple times.
  void stop() {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
      // already stopping/stopped
    }
    drain();
    for (auto &t : threads_) {
      if (t.joinable())
        t.join();
    }
    threads_.clear();
  }

  std::size_t approx_queue_size() const {
    std::size_t total = 0;
    for (auto &ptr : queues_) {
      auto &qd = *ptr;
      std::lock_guard<std::mutex> lock(qd.m);
      total += qd.high.size() + qd.normal.size() + qd.low.size();
    }
    return total;
  }

  std::size_t thread_count() const { return threads_.size(); }
  std::size_t outstanding() const {
    return outstanding_.load(std::memory_order_acquire);
  }

private:
  struct QueueData {
    std::deque<Job> high;
    std::deque<Job> normal;
    std::deque<Job> low;
    mutable std::mutex m;
  };

  static std::deque<Job> &dequeFor(QueueData &qd, Priority pr) {
    switch (pr) {
    case Priority::High:
      return qd.high;
    case Priority::Low:
      return qd.low;
    default:
      return qd.normal;
    }
  }

  bool popJob(std::size_t idx, Job &out) {
    // Try local queues
    {
      auto &qd = *queues_[idx];
      std::lock_guard<std::mutex> lock(qd.m);
      if (!qd.high.empty()) {
        out = std::move(qd.high.back());
        qd.high.pop_back();
        return true;
      }
      if (!qd.normal.empty()) {
        out = std::move(qd.normal.back());
        qd.normal.pop_back();
        return true;
      }
      if (!qd.low.empty()) {
        out = std::move(qd.low.back());
        qd.low.pop_back();
        return true;
      }
    }
    // Steal from others
    for (std::size_t n = 0; n < queues_.size(); ++n) {
      std::size_t victim = (idx + 1 + n) % queues_.size();
      if (victim == idx)
        continue;
      auto &vq = *queues_[victim];
      std::lock_guard<std::mutex> lock(vq.m);
      if (!vq.high.empty()) {
        out = std::move(vq.high.front());
        vq.high.pop_front();
        return true;
      }
      if (!vq.normal.empty()) {
        out = std::move(vq.normal.front());
        vq.normal.pop_front();
        return true;
      }
      if (!vq.low.empty()) {
        out = std::move(vq.low.front());
        vq.low.pop_front();
        return true;
      }
    }
    return false;
  }

  void workerLoop(std::size_t idx) {
    for (;;) {
      Job job;
      if (popJob(idx, job)) {
        active_.fetch_add(1, std::memory_order_acq_rel);
        if (job.fn)
          job.fn();
        active_.fetch_sub(1, std::memory_order_acq_rel);
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        continue;
      }
      if (stopping_.load(std::memory_order_acquire) &&
          outstanding_.load(std::memory_order_acquire) == 0) {
        break;
      }
      std::this_thread::yield();
    }
  }

  std::vector<std::unique_ptr<QueueData>> queues_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::size_t> active_{0};
  std::atomic<std::size_t> outstanding_{0};
  std::size_t maxOutstanding_ = 0;
  [[maybe_unused]] bool verbose_{false};
  std::atomic<std::size_t> nextWorker_{0};
};

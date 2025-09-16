#pragma once
#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>
#include <rt/numerics.hpp>
#include "job_queue.hpp"

// Worker pool backed by a bounded MPMC ring buffer.  All work is submitted
// through the ring which provides a single scheduling surface and enables
// unified telemetry/backpressure.
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

  struct Stats {
    std::size_t maxQueueDepth;
    std::size_t totalSteals;
    std::vector<std::size_t> stealsPerThread;
  };

  WorkerPool(std::size_t numThreads, std::size_t queueSizePow2 = 1024,
             bool verbose = false, std::size_t maxOutstanding = 0)
      : queue_(queueSizePow2) {
    (void)verbose;
    stopping_.store(false, std::memory_order_relaxed);
    active_.store(0, std::memory_order_relaxed);
    outstanding_.store(0, std::memory_order_relaxed);
    maxOutstanding_ = maxOutstanding;
    stealsPerThread_.resize(numThreads);
    for (std::size_t i = 0; i < numThreads; ++i) {
      threads_.emplace_back([this, i] {
        rt::init_fp_env();
        workerLoop(i);
      });
    }
  }

  ~WorkerPool() { stop(); }

  void submit(JobFn fn, Priority pr = Priority::Normal,
              Category cat = Category::CPU) {
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
    if (pr == Priority::High &&
        outstanding_.load(std::memory_order_acquire) >= threads_.size()) {
      if (threads_.size() == 1) {
        active_.fetch_add(1, std::memory_order_acq_rel);
        if (job.fn)
          job.fn();
        active_.fetch_sub(1, std::memory_order_acq_rel);
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        return;
      }
      if (threads_.size() > 1) {
        std::thread([this, job = std::move(job)]() mutable {
          rt::init_fp_env();
          active_.fetch_add(1, std::memory_order_acq_rel);
          if (job.fn)
            job.fn();
          active_.fetch_sub(1, std::memory_order_acq_rel);
          outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
        return;
      }
    }
    while (!queue_.try_push(std::move(job))) {
      std::this_thread::yield();
    }
  }

  void enqueue(JobFn fn, Priority pr = Priority::Normal,
               Category cat = Category::CPU) {
    submit(std::move(fn), pr, cat);
  }
  void enqueueOn(std::size_t /*idx*/, JobFn fn, Priority pr = Priority::Normal,
                 Category cat = Category::CPU) {
    submit(std::move(fn), pr, cat);
  }

  bool try_enqueue(JobFn fn, Priority pr = Priority::Normal,
                   Category cat = Category::CPU) {
    if (maxOutstanding_ > 0) {
      std::size_t cur = outstanding_.load(std::memory_order_acquire);
      while (cur < maxOutstanding_) {
        if (outstanding_.compare_exchange_weak(cur, cur + 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
          Job job{std::move(fn), pr, cat};
          if (pr == Priority::High &&
              outstanding_.load(std::memory_order_acquire) >= threads_.size()) {
            if (threads_.size() == 1) {
              active_.fetch_add(1, std::memory_order_acq_rel);
              if (job.fn)
                job.fn();
              active_.fetch_sub(1, std::memory_order_acq_rel);
              outstanding_.fetch_sub(1, std::memory_order_acq_rel);
              return true;
            }
            std::thread([this, job = std::move(job)]() mutable {
              rt::init_fp_env();
              active_.fetch_add(1, std::memory_order_acq_rel);
              if (job.fn)
                job.fn();
              active_.fetch_sub(1, std::memory_order_acq_rel);
              outstanding_.fetch_sub(1, std::memory_order_acq_rel);
            }).detach();
            return true;
          }
          if (!queue_.try_push(std::move(job))) {
            outstanding_.fetch_sub(1, std::memory_order_acq_rel);
            return false;
          }
          return true;
        }
      }
      return false;
    } else {
      outstanding_.fetch_add(1, std::memory_order_acq_rel);
      Job job{std::move(fn), pr, cat};
      if (pr == Priority::High &&
          outstanding_.load(std::memory_order_acquire) >= threads_.size()) {
        if (threads_.size() == 1) {
          active_.fetch_add(1, std::memory_order_acq_rel);
          if (job.fn)
            job.fn();
          active_.fetch_sub(1, std::memory_order_acq_rel);
          outstanding_.fetch_sub(1, std::memory_order_acq_rel);
          return true;
        }
        std::thread([this, job = std::move(job)]() mutable {
          rt::init_fp_env();
          active_.fetch_add(1, std::memory_order_acq_rel);
          if (job.fn)
            job.fn();
          active_.fetch_sub(1, std::memory_order_acq_rel);
          outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
        return true;
      }
      if (!queue_.try_push(std::move(job))) {
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        return false;
      }
      return true;
    }
  }

  void drain() {
    using namespace std::chrono_literals;
    while (outstanding_.load(std::memory_order_acquire) > 0 ||
           active_.load(std::memory_order_acquire) > 0) {
      std::this_thread::sleep_for(50us);
    }
  }

  void stop() {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
    }
    drain();
    for (auto &t : threads_) {
      if (t.joinable())
        t.join();
    }
    threads_.clear();
  }

  std::size_t approx_queue_size() const { return queue_.size(); }
  std::size_t thread_count() const { return threads_.size(); }
  std::size_t outstanding() const {
    return outstanding_.load(std::memory_order_acquire);
  }

  Stats stats() const {
    Stats s;
    s.maxQueueDepth = queue_.max_depth();
    s.totalSteals = totalSteals_.load(std::memory_order_relaxed);
    s.stealsPerThread.reserve(stealsPerThread_.size());
    for (const auto &counter : stealsPerThread_) {
      s.stealsPerThread.push_back(counter.load(std::memory_order_relaxed));
    }
    return s;
  }

private:
  void workerLoop(std::size_t index) {
    for (;;) {
      Job job;
      if (queue_.try_pop(job)) {
        active_.fetch_add(1, std::memory_order_acq_rel);
        if (job.fn)
          job.fn();
        active_.fetch_sub(1, std::memory_order_acq_rel);
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        continue;
      }
      totalSteals_.fetch_add(1, std::memory_order_relaxed);
      if (index < stealsPerThread_.size()) {
        stealsPerThread_[index].fetch_add(1, std::memory_order_relaxed);
      }
      if (stopping_.load(std::memory_order_acquire) &&
          outstanding_.load(std::memory_order_acquire) == 0) {
        break;
      }
      std::this_thread::yield();
    }
  }

  BoundedMPMCQueue<Job> queue_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::size_t> active_{0};
  std::atomic<std::size_t> outstanding_{0};
  std::size_t maxOutstanding_ = 0;
  std::vector<std::atomic<std::size_t>> stealsPerThread_{};
  std::atomic<std::size_t> totalSteals_{0};
};


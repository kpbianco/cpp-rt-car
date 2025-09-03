#pragma once
#include "job_queue.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

// Simple persistent worker pool built on the lock-free queue.
// Jobs are std::function<void()>.
//
// Usage:
//   WorkerPool pool(threads, 1024);
//   pool.enqueue([]{ ... });
//   pool.drain();   // wait until all queued jobs finish
//   pool.stop();    // join threads (optional; destructor does this too)
//
class WorkerPool {
public:
  using Job = std::function<void()>;

  WorkerPool(std::size_t numThreads, std::size_t queueSizePow2 = 1024,
             bool verbose = false, std::size_t maxOutstanding = 0)
      : queue_(queueSizePow2), verbose_(verbose) {
    stopping_.store(false, std::memory_order_relaxed);
    active_.store(0, std::memory_order_relaxed);
    outstanding_.store(0, std::memory_order_relaxed);
    maxOutstanding_ = maxOutstanding ? maxOutstanding : queue_.capacity();
    threads_.reserve(numThreads);
    for (std::size_t i = 0; i < numThreads; ++i) {
      threads_.emplace_back([this] { this->workerLoop(); });
    }
  }

  ~WorkerPool() { stop(); }

  // Busy-wait enqueue (lock-free); returns after the job is on the queue.
  void enqueue(Job j) {
    // Reserve an outstanding slot, blocking until under the limit
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
    while (!queue_.try_push(std::move(j))) {
      // Backoff to reduce contention if the queue is temporarily full
      std::this_thread::yield();
    }
  }

  // Non-blocking variant; returns false if full.
  bool try_enqueue(Job j) {
    if (maxOutstanding_ > 0) {
      std::size_t cur = outstanding_.load(std::memory_order_acquire);
      while (cur < maxOutstanding_) {
        if (outstanding_.compare_exchange_weak(cur, cur + 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
          if (queue_.try_push(std::move(j))) {
            return true;
          }
          // queue full; undo reservation
          outstanding_.fetch_sub(1, std::memory_order_acq_rel);
          return false;
        }
      }
      return false;
    } else {
      if (!queue_.try_push(std::move(j)))
        return false;
      outstanding_.fetch_add(1, std::memory_order_acq_rel);
      return true;
    }
  }

  // Wait until queue is empty and all workers are idle.
  void drain() {
    using namespace std::chrono_literals;
    for (;;) {
      if (queue_.empty() && active_.load(std::memory_order_acquire) == 0)
        break;
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
    // Give workers a chance to exit when idle; also drain any stragglers
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

private:
  void workerLoop() {
    for (;;) {
      Job job;
      if (queue_.try_pop(job)) {
        active_.fetch_add(1, std::memory_order_acq_rel);
        // Guard against empty std::function (shouldn't happen, but be safe)
        if (job)
          job();
        else if (verbose_) { /*no-op*/
        }
        active_.fetch_sub(1, std::memory_order_acq_rel);
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        continue;
      }
      if (stopping_.load(std::memory_order_acquire)) {
        // If asked to stop and nothing to do, exit.
        if (queue_.empty() && active_.load(std::memory_order_acquire) == 0)
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
  bool verbose_{false};
};

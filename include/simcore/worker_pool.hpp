#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>
#include <rt/arch.hpp>
#include <rt/numerics.hpp>
#include "bintrace.hpp"
#include "job_queue.hpp"
#include "debug.hpp"

#ifndef RTFW_DISABLE_EMERGENCY_SPAWN
#define RTFW_DISABLE_EMERGENCY_SPAWN 0
#endif

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
    std::uint64_t emergencySpawns;
  };

  WorkerPool(std::size_t numThreads, std::size_t queueSizePow2 = 1024,
             bool verbose = false, std::size_t maxOutstanding = 0)
      : queue_(queueSizePow2), stealsPerThread_(numThreads) {
    (void)verbose;
    stopping_.store(false, std::memory_order_relaxed);
    active_.store(0, std::memory_order_relaxed);
    outstanding_.store(0, std::memory_order_relaxed);
    maxOutstanding_ = maxOutstanding;
    emergencyLastRefill_ = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < numThreads; ++i) {
      simcore::debug::assert_thread_creation_allowed();
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
          rt::cpu_relax();
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
#if RTFW_DISABLE_EMERGENCY_SPAWN
    bool redirectedToPriorityLane = false;
    std::size_t redirectedOutstanding = 0;
#endif
    if (pr == Priority::High) {
      const std::size_t outstandingCount =
          outstanding_.load(std::memory_order_acquire);
      if (outstandingCount >= threads_.size()) {
#if RTFW_DISABLE_EMERGENCY_SPAWN
        redirectedToPriorityLane = true;
        redirectedOutstanding = outstandingCount;
#else
        if (handleEmergency(job, outstandingCount)) {
          return;
        }
#endif
      }
    }
    while (true) {
      if (queue_.try_push(std::move(job))) {
#if RTFW_DISABLE_EMERGENCY_SPAWN
        if (redirectedToPriorityLane) {
          logPriorityEnqueue(redirectedOutstanding, job.priority,
                             job.category);
        }
#endif
        break;
      }
      rt::cpu_relax();
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
#if RTFW_DISABLE_EMERGENCY_SPAWN
          bool redirectedToPriorityLane = false;
          std::size_t redirectedOutstanding = 0;
#endif
          Job job{std::move(fn), pr, cat};
          if (pr == Priority::High) {
            const std::size_t outstandingCount =
                outstanding_.load(std::memory_order_acquire);
            if (outstandingCount >= threads_.size()) {
#if RTFW_DISABLE_EMERGENCY_SPAWN
              redirectedToPriorityLane = true;
              redirectedOutstanding = outstandingCount;
#else
              if (handleEmergency(job, outstandingCount)) {
                return true;
              }
#endif
            }
          }
          if (!queue_.try_push(std::move(job))) {
            outstanding_.fetch_sub(1, std::memory_order_acq_rel);
            return false;
          }
#if RTFW_DISABLE_EMERGENCY_SPAWN
          if (redirectedToPriorityLane) {
            logPriorityEnqueue(redirectedOutstanding, job.priority,
                               job.category);
          }
#endif
          return true;
        }
      }
      return false;
    } else {
      outstanding_.fetch_add(1, std::memory_order_acq_rel);
      Job job{std::move(fn), pr, cat};
#if RTFW_DISABLE_EMERGENCY_SPAWN
      bool redirectedToPriorityLane = false;
      std::size_t redirectedOutstanding = 0;
#endif
      if (pr == Priority::High) {
        const std::size_t outstandingCount =
            outstanding_.load(std::memory_order_acquire);
        if (outstandingCount >= threads_.size()) {
#if RTFW_DISABLE_EMERGENCY_SPAWN
          redirectedToPriorityLane = true;
          redirectedOutstanding = outstandingCount;
#else
          if (handleEmergency(job, outstandingCount)) {
            return true;
          }
#endif
        }
      }
      if (!queue_.try_push(std::move(job))) {
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        return false;
      }
#if RTFW_DISABLE_EMERGENCY_SPAWN
      if (redirectedToPriorityLane) {
        logPriorityEnqueue(redirectedOutstanding, job.priority,
                           job.category);
      }
#endif
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

  void setTrace(bintrace::Trace *trace, std::size_t baseIndex) {
    traceBase_.store(baseIndex, std::memory_order_release);
    trace_.store(trace, std::memory_order_release);
  }

  Stats stats() const {
    Stats s;
    s.maxQueueDepth = queue_.max_depth();
    s.stealsPerThread.reserve(stealsPerThread_.size());
    std::size_t totalSteals = 0;
    for (const auto &counter : stealsPerThread_) {
      auto steals = counter.load(std::memory_order_relaxed);
      s.stealsPerThread.push_back(steals);
      totalSteals += steals;
    }
    s.totalSteals = totalSteals;
    s.emergencySpawns =
        emergencySpawns_.load(std::memory_order_acquire);
    return s;
  }

private:
  static constexpr double kEmergencySpawnRatePerSecond = 2.0;
  static constexpr double kEmergencySpawnCapacity = 2.0;

  bool tryConsumeEmergencyToken() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(emergencyMutex_);
    const auto elapsed =
        std::chrono::duration<double>(now - emergencyLastRefill_).count();
    if (elapsed > 0.0) {
      emergencyTokens_ =
          std::min(kEmergencySpawnCapacity,
                   emergencyTokens_ + elapsed * kEmergencySpawnRatePerSecond);
      emergencyLastRefill_ = now;
    }
    if (emergencyTokens_ >= 1.0) {
      emergencyTokens_ -= 1.0;
      return true;
    }
    return false;
  }

  bool handleEmergency(Job &job, std::size_t outstandingCount) {
#if RTFW_DISABLE_EMERGENCY_SPAWN
    (void)job;
    (void)outstandingCount;
    return false;
#else
    if (threads_.size() == 1) {
      active_.fetch_add(1, std::memory_order_acq_rel);
      if (job.fn)
        job.fn();
      active_.fetch_sub(1, std::memory_order_acq_rel);
      outstanding_.fetch_sub(1, std::memory_order_acq_rel);
      return true;
    }
    if (threads_.size() <= 1)
      return false;

    const bool rateAllowed = tryConsumeEmergencyToken();
    logEmergencySpawn(outstandingCount, rateAllowed, job.priority,
                      job.category);
    if (!rateAllowed)
      return false;

    simcore::debug::assert_thread_creation_allowed();
    emergencySpawns_.fetch_add(1, std::memory_order_acq_rel);
    std::thread([this, job = std::move(job)]() mutable {
      rt::init_fp_env();
      active_.fetch_add(1, std::memory_order_acq_rel);
      if (job.fn)
        job.fn();
      active_.fetch_sub(1, std::memory_order_acq_rel);
      outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    }).detach();
    return true;
#endif
  }

  static constexpr std::uint32_t encodePriority(Priority priority) {
    switch (priority) {
    case Priority::Low:
      return 0u;
    case Priority::Normal:
      return 1u;
    case Priority::High:
      return 2u;
    }
    return 0u;
  }

  static constexpr std::uint32_t encodeCategory(Category category) {
    switch (category) {
    case Category::CPU:
      return 0u;
    case Category::GPU:
      return 1u;
    case Category::IO:
      return 2u;
    }
    return 0u;
  }

  static constexpr std::uint64_t
  encodeEmergencySpawnPayload(bool rateAllowed, Priority priority,
                              Category category) {
    const std::uint64_t priorityBits =
        static_cast<std::uint64_t>(encodePriority(priority));
    const std::uint64_t categoryBits =
        static_cast<std::uint64_t>(encodeCategory(category)) << 32;
    const std::uint64_t rateBit = rateAllowed ? (1ull << 63) : 0ull;
    return rateBit | categoryBits | priorityBits;
  }

  void logEmergencySpawn(std::size_t outstandingCount, bool rateAllowed,
                         Priority priority, Category category) {
    if (auto *trace = trace_.load(std::memory_order_acquire)) {
      const std::uint32_t outstandingTruncated =
          outstandingCount > std::numeric_limits<std::uint32_t>::max()
              ? std::numeric_limits<std::uint32_t>::max()
              : static_cast<std::uint32_t>(outstandingCount);
      const std::uint64_t payload =
          encodeEmergencySpawnPayload(rateAllowed, priority, category);
      trace->log(bintrace::EV_EmergencySpawn, outstandingTruncated, payload);
    }
  }

  static constexpr std::uint64_t
  encodePriorityEnqueuePayload(Priority priority, Category category) {
    const std::uint64_t priorityBits =
        static_cast<std::uint64_t>(encodePriority(priority));
    const std::uint64_t categoryBits =
        static_cast<std::uint64_t>(encodeCategory(category)) << 32;
    return priorityBits | categoryBits;
  }

  void logPriorityEnqueue(std::size_t outstandingCount, Priority priority,
                          Category category) {
    if (auto *trace = trace_.load(std::memory_order_acquire)) {
      const std::uint32_t outstandingTruncated =
          outstandingCount > std::numeric_limits<std::uint32_t>::max()
              ? std::numeric_limits<std::uint32_t>::max()
              : static_cast<std::uint32_t>(outstandingCount);
      const std::uint64_t payload =
          encodePriorityEnqueuePayload(priority, category);
      trace->log(bintrace::EV_PriorityEnqueue, outstandingTruncated, payload);
    }
  }

  void workerLoop(std::size_t index) {
    bool bound = false;
    bool stealLogged = false;
    for (;;) {
      if (!bound) {
        if (auto *trace = trace_.load(std::memory_order_acquire)) {
          const std::size_t base = traceBase_.load(std::memory_order_acquire);
          trace->bindThread(base + index);
          bound = true;
        }
      }
      Job job;
      if (queue_.try_pop(job)) {
        active_.fetch_add(1, std::memory_order_acq_rel);
        if (job.fn)
          job.fn();
        active_.fetch_sub(1, std::memory_order_acq_rel);
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        stealLogged = false;
        continue;
      }

      const bool hasOutstanding =
          outstanding_.load(std::memory_order_acquire) > 0;
      if (hasOutstanding && index < stealsPerThread_.size()) {
        stealsPerThread_[index].fetch_add(1, std::memory_order_relaxed);
      }

      if (auto *trace = trace_.load(std::memory_order_acquire)) {
        if (hasOutstanding) {
          if (!stealLogged) {
            trace->log(bintrace::EV_WorkSteal,
                       static_cast<std::uint32_t>(index),
                       static_cast<std::uint64_t>(queue_.size()));
            stealLogged = true;
          }
        } else {
          stealLogged = false;
        }
      } else if (!hasOutstanding) {
        stealLogged = false;
      }

      if (stopping_.load(std::memory_order_acquire) && !hasOutstanding) {
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
  std::atomic<bintrace::Trace *> trace_{nullptr};
  std::atomic<std::size_t> traceBase_{0};
  std::mutex emergencyMutex_;
  double emergencyTokens_ = kEmergencySpawnCapacity;
  std::chrono::steady_clock::time_point emergencyLastRefill_{};
  std::atomic<std::uint64_t> emergencySpawns_{0};
};


#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include <rt/arch.hpp>

#include "bintrace.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
// Disable "structure was padded due to alignment specifier" which fires on
// the cache-line alignment used throughout the queue implementation.
#pragma warning(disable : 4324)
#endif

// Lock-free bounded multi-producer/multi-consumer queue based on
// Dmitry Vyukov's algorithm.  Adds sequence-number padding to avoid
// false sharing, uses per-thread caches for hot loops and provides a
// fast path for single-producer scenarios.  Sequence numbers also
// protect against ABA issues when slots wrap around.

template <typename T> class BoundedMPMCQueue {
public:
  explicit BoundedMPMCQueue(std::size_t capacityPow2)
      : buffer_(nextPow2(capacityPow2 ? capacityPow2 : 1)),
        mask_(buffer_.size() - 1) {
    for (std::size_t i = 0; i < buffer_.size(); ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
    enqueuePos_.store(0, std::memory_order_relaxed);
    dequeuePos_.store(0, std::memory_order_relaxed);
  }

  // Non-blocking push; returns false if the queue is full.  When
  // singleProducer is true the enqueue position is updated via a
  // relaxed store allowing hot producer loops to avoid a CAS.  Accepts
  // both lvalues and rvalues via perfect forwarding.
  template <typename U> bool try_push(U &&v, bool singleProducer = false) {
    std::size_t pos = singleProducer
                          ? enqueueCache_
                          : enqueuePos_.load(std::memory_order_relaxed);
    Cell *cell;
    for (;;) {
      cell = &buffer_[pos & mask_];
      std::size_t seq = cell->sequence.load(std::memory_order_acquire);
      std::ptrdiff_t dif =
          static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
      if (dif == 0) {
        if (singleProducer) {
          enqueuePos_.store(pos + 1, std::memory_order_relaxed);
          break;
        }
        if (enqueuePos_.compare_exchange_weak(pos, pos + 1,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
          break;
        }
      } else if (dif < 0) {
        enqueueCache_ = pos;
        return false; // full
      } else {
        pos = enqueuePos_.load(std::memory_order_relaxed);
      }
      rt::cpu_relax();
    }
    enqueueCache_ = pos + 1;
    cell->data = std::forward<U>(v);
    cell->sequence.store(pos + 1, std::memory_order_release);
    auto prevCount = count_.fetch_add(1, std::memory_order_acq_rel);
    auto depth = prevCount + 1;
    auto prev = maxDepth_.load(std::memory_order_relaxed);
    while (depth > prev &&
           !maxDepth_.compare_exchange_weak(prev, depth,
                                            std::memory_order_relaxed)) {
    }
    if (depth <= std::numeric_limits<std::uint32_t>::max()) {
        bintrace::log_queue_push(static_cast<std::uint32_t>(depth),
                                 static_cast<std::uint64_t>(mask_ + 1));
    }
    return true;
  }

  // Non-blocking pop; returns false if the queue is empty.
  bool try_pop(T &out) {
    std::size_t pos = dequeueCache_;
    Cell *cell;
    for (;;) {
      cell = &buffer_[pos & mask_];
      std::size_t seq = cell->sequence.load(std::memory_order_acquire);
      std::ptrdiff_t dif = static_cast<std::ptrdiff_t>(seq) -
                           static_cast<std::ptrdiff_t>(pos + 1);
      if (dif == 0) {
        if (dequeuePos_.compare_exchange_weak(pos, pos + 1,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
          break;
        }
      } else if (dif < 0) {
        dequeueCache_ = pos;
        return false; // empty
      } else {
        pos = dequeuePos_.load(std::memory_order_relaxed);
      }
      rt::cpu_relax();
    }
    dequeueCache_ = pos + 1;
    out = std::move(cell->data);
    cell->sequence.store(pos + buffer_.size(), std::memory_order_release);
    auto prevCount = count_.fetch_sub(1, std::memory_order_acq_rel);
    auto depth = (prevCount > 0) ? (prevCount - 1) : 0;
    if (depth <= std::numeric_limits<std::uint32_t>::max()) {
        bintrace::log_queue_pop(static_cast<std::uint32_t>(depth),
                                static_cast<std::uint64_t>(mask_ + 1));
    }
    return true;
  }

  std::size_t capacity() const { return buffer_.size(); }

  // Convenience alias for telemetry hooks.  The worker pool records peak
  // queue depth by sampling this value, so expose an explicit name that makes
  // the intent clear to readers of the instrumentation code.
  std::size_t max_capacity() const { return buffer_.size(); }

  std::size_t size() const {
    return count_.load(std::memory_order_acquire);
  }

  bool empty() const { return size() == 0; }

  std::size_t max_depth() const {
    return maxDepth_.load(std::memory_order_relaxed);
  }

private:
  static constexpr std::size_t cacheLine() {
#if defined(__cpp_lib_hardware_interference_size) && \
    !(defined(__GNUC__) && !defined(__clang__))
    return std::hardware_destructive_interference_size;
#else
    // GCC emits -Winterference-size even though the feature test macro is
    // defined, so fall back to a conservative cache line size there.
    return 64;
#endif
  }

  struct alignas(cacheLine()) Cell {
    std::atomic<std::size_t> sequence;
    T data;
  };

  static std::size_t nextPow2(std::size_t x) {
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    return x + 1;
  }

  std::vector<Cell> buffer_;
  const std::size_t mask_;
  alignas(cacheLine()) std::atomic<std::size_t> enqueuePos_{0};
  alignas(cacheLine()) std::atomic<std::size_t> dequeuePos_{0};
  std::atomic<std::size_t> count_{0};
  std::atomic<std::size_t> maxDepth_{0};

  // Per-thread caches for enqueue/dequeue positions.
  static thread_local std::size_t enqueueCache_;
  static thread_local std::size_t dequeueCache_;
};

template <typename T>
thread_local std::size_t BoundedMPMCQueue<T>::enqueueCache_ = 0;

template <typename T>
thread_local std::size_t BoundedMPMCQueue<T>::dequeueCache_ = 0;

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

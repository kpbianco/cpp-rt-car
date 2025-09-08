#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

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
  // relaxed store allowing hot producer loops to avoid a CAS.
  bool try_push(T &&v, bool singleProducer = false) {
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
    }
    enqueueCache_ = pos + 1;
    cell->data = std::move(v);
    cell->sequence.store(pos + 1, std::memory_order_release);
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
    }
    dequeueCache_ = pos + 1;
    out = std::move(cell->data);
    cell->sequence.store(pos + buffer_.size(), std::memory_order_release);
    return true;
  }

  std::size_t capacity() const { return buffer_.size(); }

  std::size_t size() const {
    auto enq = enqueuePos_.load(std::memory_order_acquire);
    auto deq = dequeuePos_.load(std::memory_order_acquire);
    return enq - deq;
  }

  bool empty() const { return size() == 0; }

private:
  static constexpr std::size_t cacheLine() {
#ifdef __cpp_lib_hardware_interference_size
    return std::hardware_destructive_interference_size;
#else
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

  // Per-thread caches for enqueue/dequeue positions.
  static thread_local std::size_t enqueueCache_;
  static thread_local std::size_t dequeueCache_;
};

template <typename T>
thread_local std::size_t BoundedMPMCQueue<T>::enqueueCache_ = 0;

template <typename T>
thread_local std::size_t BoundedMPMCQueue<T>::dequeueCache_ = 0;

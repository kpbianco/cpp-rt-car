#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory> // unique_ptr
#include <new>
#include <type_traits>

//
// Bounded MPMC queue (Dmitry Vyukov algorithm).
// - capacity must be a power of two (we round up if needed)
// - try_push / try_pop are lock-free
// - size() is approximate
//
template <typename T> class BoundedMPMCQueue {
public:
  explicit BoundedMPMCQueue(std::size_t capacityPow2) {
    const std::size_t cap = nextPow2(capacityPow2 ? capacityPow2 : 1);
    capacity_ = cap;
    mask_ = cap - 1;

    // allocate cells without std::vector to avoid move/copy constraints
    buffer_.reset(new Cell[cap]);
    for (std::size_t i = 0; i < cap; ++i) {
      buffer_[i].seq.store(i, std::memory_order_relaxed);
        for (std::size_t i = 0; i < cap; ++i) {
            buffer_[i].seq.store(i, std::memory_order_relaxed);
        }
        head_.value.store(0, std::memory_order_relaxed);
        tail_.value.store(0, std::memory_order_relaxed);
        sz_.value.store(0,   std::memory_order_relaxed);
    }
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    sz_.store(0, std::memory_order_relaxed);
  }

  ~BoundedMPMCQueue() {
    // Drain remaining items so their destructors run
    T tmp;
    while (try_pop(tmp)) {
    }
  }

  // Non-blocking push; returns false if full
  bool try_push(T &&v) {
    Cell *cell;
    std::size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      cell = &buffer_[pos & mask_];
      std::size_t seq = cell->seq.load(std::memory_order_acquire);
      intptr_t dif = (intptr_t)seq - (intptr_t)pos;
      if (dif == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
          break;
        }
      } else if (dif < 0) {
        return false; // full
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    // Non-blocking push; returns false if full
    bool try_push(T&& v) {
        Cell* cell;
        std::size_t pos = tail_.value.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            std::size_t seq = cell->seq.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)pos;
            if (dif == 0) {
                if (tail_.value.compare_exchange_weak(pos, pos+1,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false; // full
            } else {
                pos = tail_.value.load(std::memory_order_relaxed);
            }
        }
        ::new (&cell->storage) T(std::move(v));
        cell->seq.store(pos + 1, std::memory_order_release);
        sz_.value.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    ::new (&cell->storage) T(std::move(v));
    cell->seq.store(pos + 1, std::memory_order_release);
    // Track number of enqueued items; acq_rel so external observers see
    // up-to-date counts
    sz_.fetch_add(1, std::memory_order_acq_rel);
    return true;
  }

  // Non-blocking pop; returns false if empty
  bool try_pop(T &out) {
    Cell *cell;
    std::size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      cell = &buffer_[pos & mask_];
      std::size_t seq = cell->seq.load(std::memory_order_acquire);
      intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
      if (dif == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
          break;
        }
      } else if (dif < 0) {
        return false; // empty
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    // Non-blocking pop; returns false if empty
    bool try_pop(T& out) {
        Cell* cell;
        std::size_t pos = head_.value.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            std::size_t seq = cell->seq.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
            if (dif == 0) {
                if (head_.value.compare_exchange_weak(pos, pos+1,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false; // empty
            } else {
                pos = head_.value.load(std::memory_order_relaxed);
            }
        }
        T* ptr = reinterpret_cast<T*>(&cell->storage);
        out = std::move(*ptr);
        ptr->~T();
        cell->seq.store(pos + mask_ + 1, std::memory_order_release);
        sz_.value.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }
    T *ptr = reinterpret_cast<T *>(&cell->storage);
    out = std::move(*ptr);
    ptr->~T();
    cell->seq.store(pos + mask_ + 1, std::memory_order_release);
    // Decrement outstanding item count
    sz_.fetch_sub(1, std::memory_order_acq_rel);
    return true;
  }

  std::size_t capacity() const { return capacity_; }
  std::size_t size() const { return sz_.load(std::memory_order_acquire); }
  bool empty() const { return sz_.load(std::memory_order_acquire) == 0; }
=======
    std::size_t capacity() const { return capacity_; }
    std::size_t size()     const { return sz_.value.load(std::memory_order_relaxed); }
    bool        empty()    const { return size() == 0; }

private:
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

  struct Cell {
    std::atomic<std::size_t> seq;
    typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;
    struct alignas(64) Cell {
        std::atomic<std::size_t> seq;
        typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;

    Cell() noexcept : seq(0) {}
    Cell(const Cell &) = delete;
    Cell &operator=(const Cell &) = delete;
    Cell(Cell &&) = delete;
    Cell &operator=(Cell &&) = delete;
  };

  std::unique_ptr<Cell[]> buffer_{};
  std::size_t capacity_ = 0;
  std::size_t mask_ = 0;
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
  std::atomic<std::size_t> sz_{0};
    std::unique_ptr<Cell[]>    buffer_{};
    std::size_t                capacity_ = 0;
    std::size_t                mask_     = 0;

    // Place frequently-mutated counters on separate cache lines to reduce
    // false sharing between producers and consumers.
    struct alignas(64) PaddedAtomic {
        std::atomic<std::size_t> value;
        PaddedAtomic(std::size_t v = 0) noexcept : value(v) {}
    };

    PaddedAtomic head_{0};
    PaddedAtomic tail_{0};
    PaddedAtomic sz_{0};
};

#pragma once
#include <atomic>
#include <type_traits>

namespace simcore {

// Simple seqlock implementation for read-mostly data.
// Writers obtain exclusive access by incrementing the sequence to an odd value.
// Readers retry if sequence changes during read.

template <typename T> class SeqLock {
  static_assert(std::is_trivially_copyable_v<T>, "SeqLock requires POD type");

  std::atomic<std::uint64_t> seq_{0};
  T data_{};

public:
  SeqLock() = default;
  explicit SeqLock(const T &initial) : data_(initial) {}

  void write(const T &v) {
    std::uint64_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_release); // make odd
    data_ = v;
    seq_.store(s + 2, std::memory_order_release); // make even
  }

  T read() const {
    T snapshot;
    while (true) {
      std::uint64_t s1 = seq_.load(std::memory_order_acquire);
      if (s1 & 1)
        continue; // writer in progress
      snapshot = data_;
      std::uint64_t s2 = seq_.load(std::memory_order_acquire);
      if (s1 == s2)
        break; // consistent
    }
    return snapshot;
  }
};

} // namespace simcore

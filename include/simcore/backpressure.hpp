#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>

namespace simcore {

// Token bucket for admission control & pacing. Thread-safe using atomics.
class TokenBucket {
  using Clock = std::chrono::steady_clock;
  const int capacity_;
  const double refill_rate_; // tokens per second
  std::atomic<int> tokens_;
  std::atomic<std::int64_t> last_ns_;

  static std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
  }

  void refill() {
    std::int64_t prev = last_ns_.load(std::memory_order_relaxed);
    std::int64_t cur = now_ns();
    double elapsed = (cur - prev) / 1e9;
    if (elapsed <= 0)
      return;
    if (last_ns_.compare_exchange_strong(prev, cur)) {
      int add = static_cast<int>(elapsed * refill_rate_);
      if (add > 0) {
        int t = tokens_.load(std::memory_order_relaxed);
        int new_t;
        do {
          new_t = std::min(capacity_, t + add);
        } while (!tokens_.compare_exchange_weak(t, new_t));
      }
    }
  }

public:
  TokenBucket(int capacity, double refill_rate)
      : capacity_(capacity), refill_rate_(refill_rate), tokens_(capacity),
        last_ns_(now_ns()) {}

  bool try_acquire(int n = 1) {
    refill();
    int t = tokens_.load(std::memory_order_relaxed);
    while (t >= n) {
      if (tokens_.compare_exchange_weak(t, t - n))
        return true;
    }
    return false;
  }
};

} // namespace simcore

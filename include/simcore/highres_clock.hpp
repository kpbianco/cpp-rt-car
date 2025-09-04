#pragma once
#include <atomic>
#include <cstdint>
#include <chrono>
#include <thread>
#include <functional>

// High resolution monotonic clock with optional TSC backing.
// Calibrates TSC frequency at init and falls back to std::chrono on drift.
class HighResClock {
public:
    using tsc_reader_t = uint64_t (*)();

    // Initialise the clock. If no reader is supplied a platform default is used.
    static void init(tsc_reader_t reader = nullptr) {
        tsc_reader_ = reader ? reader : &default_tsc_reader;
        ref_start_ns_ = to_ns(std::chrono::steady_clock::now());
        last_ns_.store(ref_start_ns_);

        // Attempt TSC calibration.
        if (tsc_reader_) {
            uint64_t t0 = tsc_reader_();
            auto ref0 = std::chrono::steady_clock::now();
            // Sleep briefly to establish frequency.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            uint64_t t1 = tsc_reader_();
            auto ref1 = std::chrono::steady_clock::now();
            auto secs = std::chrono::duration<double>(ref1 - ref0).count();
            if (secs > 0.0 && t1 > t0) {
                tsc_freq_ = static_cast<double>(t1 - t0) / secs; // cycles/sec
                tsc_start_ = t1;
                ref_start_ns_ = to_ns(ref1);
                last_ns_.store(ref_start_ns_);
                tsc_ok_.store(true);
            }
        }
    }

    // Nanoseconds since an arbitrary epoch (monotonic).
    static uint64_t now_ns() {
        auto ref_now = std::chrono::steady_clock::now();
        uint64_t ns_ref = to_ns(ref_now);

        if (tsc_ok_.load(std::memory_order_relaxed)) {
            uint64_t t = tsc_reader_();
            uint64_t ns = ref_start_ns_ +
                static_cast<uint64_t>((static_cast<double>(t - tsc_start_) * 1e9) / tsc_freq_);
            // Drift check every ~4096 calls.
            if ((++calls_ & 0xfff) == 0) {
                int64_t diff = static_cast<int64_t>(ns) - static_cast<int64_t>(ns_ref);
                if (diff > 1000000 || diff < -1000000) { // 1ms threshold
                    tsc_ok_.store(false);
                    return ensure_monotonic(ns_ref);
                }
            }
            return ensure_monotonic(ns);
        }
        return ensure_monotonic(ns_ref);
    }

    static bool using_tsc() { return tsc_ok_.load(); }
    static void set_tsc_reader(tsc_reader_t r) { tsc_reader_ = r; }

private:
    static uint64_t to_ns(std::chrono::steady_clock::time_point tp) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    }
    static uint64_t ensure_monotonic(uint64_t ns) {
        uint64_t last = last_ns_.load();
        while (ns <= last && !last_ns_.compare_exchange_weak(last, last + 1)) {
        }
        if (ns <= last) ns = last + 1;
        last_ns_.store(ns);
        return ns;
    }

    // Default TSC reader (x86); returns 0 on unsupported platforms.
    static uint64_t default_tsc_reader() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        return __rdtsc();
#elif defined(__i386__) || defined(__x86_64__)
        unsigned int lo, hi;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
#else
        return 0;
#endif
    }

    static inline std::atomic<bool> tsc_ok_{false};
    static inline tsc_reader_t tsc_reader_ = nullptr;
    static inline double tsc_freq_ = 0.0;
    static inline uint64_t tsc_start_ = 0;
    static inline uint64_t ref_start_ns_ = 0;
    static inline std::atomic<uint64_t> last_ns_{0};
    static inline std::atomic<uint64_t> calls_{0};
};


#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <atomic>
#include <cstring>
#include <cassert>
#include <new>
#include <fstream>
#include "highres_clock.hpp"

namespace bintrace {

// 32-byte fixed-size event (cache friendly, aligned)
struct alignas(32) Event {
    std::uint64_t tsc;     // TSC or monotonic fallback
    std::uint32_t code;    // user-defined code
    std::uint32_t a;       // small arg
    std::uint64_t b;       // large arg
    std::uint32_t thread;  // logical thread index
    std::uint32_t _pad{};
};

static_assert(sizeof(Event) == 32, "Trace events are expected to stay 32 bytes");

enum : std::uint32_t {
    EV_PhaseBegin        = 0x01,
    EV_PhaseEnd          = 0x02,
    EV_ChunkStart        = 0x03,
    EV_ChunkDone         = 0x04,
    EV_GovernorRung      = 0x05,
    EV_BudgetLadder      = EV_GovernorRung,
    EV_QueuePush         = 0x06,
    EV_QueuePop          = 0x07,
    EV_WorkSteal         = 0x08,
    EV_WatchdogTrip      = 0x09,
    EV_GpuFenceWaitBegin = 0x0A,
    EV_GpuFenceWaitEnd   = 0x0B,
    EV_SnapshotSave      = 0x0C,
    EV_SnapshotLoad      = 0x0D,
    EV_PlatformCrumb     = 0x0E,
    EV_EmergencySpawn    = 0x0F,
    EV_PriorityEnqueue   = 0x10,
    EV_WorkerMeta        = 0x11,
};

static inline std::uint64_t rdtsc() noexcept {
    return HighResClock::now();
}

class Trace {
public:
    Trace() = default;

    void init(std::size_t threads, std::size_t eventsPerThread, bool enabled) {
        enabled_ = enabled;
        eventsPerThread_ = eventsPerThread;
        dropped_.store(0, std::memory_order_relaxed);
        buffers_.clear();
        buffers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            auto buf = std::make_unique<Buffer>();
            if (eventsPerThread_) {
                buf->allocate(eventsPerThread_);
            }
            buffers_.emplace_back(std::move(buf));
        }
        tlsBuf_ = nullptr;
        tlsThreadIdx_ = ~std::size_t{0};
    }

    void shutdown() {
        eventsPerThread_ = 0;
        for (auto& b : buffers_) {
            if (b) {
                b->release();
            }
        }
        buffers_.clear();
        tlsBuf_ = nullptr;
        tlsThreadIdx_ = ~std::size_t{0};
        dropped_.store(0, std::memory_order_relaxed);
    }

    std::size_t threadCount() const noexcept { return buffers_.size(); }

    std::size_t appendThreads(std::size_t count) {
        if (count == 0)
            return buffers_.size();
        const std::size_t base = buffers_.size();
        buffers_.reserve(base + count);
        for (std::size_t i = 0; i < count; ++i) {
            auto buf = std::make_unique<Buffer>();
            if (eventsPerThread_) {
                buf->allocate(eventsPerThread_);
            }
            buffers_.emplace_back(std::move(buf));
        }
        return base;
    }

    bool enabled() const noexcept { return enabled_; }

    void bindThread(std::size_t idx) {
        if (idx >= buffers_.size()) return;
        tlsBuf_ = buffers_[idx].get();
        tlsThreadIdx_ = idx;
    }

    inline void log(std::uint32_t code, std::uint32_t a=0, std::uint64_t b=0) noexcept {
        if (!enabled_) return;
        Buffer* buf = tlsBuf_;
        if (!buf) return;
        const std::size_t cap = buf->cap;
        if (cap == 0) {
            recordDrop(buf);
            return;
        }
        const std::size_t i   = buf->write.load(std::memory_order_relaxed);
        Event* e = buf->base + (i % cap);
        e->tsc    = rdtsc();
        e->code   = code;
        e->a      = a;
        e->b      = b;
        e->thread = static_cast<std::uint32_t>(tlsThreadIdx_);
        buf->write.store(i + 1, std::memory_order_release);
    }

    struct Snapshot {
        std::vector<Event> events; // concatenated per-thread (not globally merged)
        std::vector<std::size_t> perThreadCount;
    };

    Snapshot snapshot() const {
        Snapshot s;
        s.perThreadCount.resize(buffers_.size(), 0);
        std::size_t total = 0;
        for (std::size_t t=0; t<buffers_.size(); ++t) {
            const Buffer& b = *buffers_[t];
            const std::size_t w = b.write.load(std::memory_order_acquire);
            std::size_t n = (w < b.cap) ? w : b.cap;
            s.perThreadCount[t] = n;
            total += n;
        }
        s.events.resize(total);

        std::size_t cursor = 0;
        for (std::size_t t=0; t<buffers_.size(); ++t) {
            const Buffer& b = *buffers_[t];
            const std::size_t w = b.write.load(std::memory_order_acquire);
            const std::size_t cap = b.cap;
            const std::size_t n = s.perThreadCount[t];
            if (n == 0) continue;
            std::size_t start = (w >= cap) ? (w % cap) : 0;
            std::size_t tail = cap - start;
            if (w < cap) tail = n;
            const std::size_t firstChunk = (tail > n) ? n : tail;
            std::memcpy(s.events.data() + cursor, b.base + start, firstChunk * sizeof(Event));
            cursor += firstChunk;
            const std::size_t remaining = n - firstChunk;
            if (remaining) {
                std::memcpy(s.events.data() + cursor, b.base, remaining * sizeof(Event));
                cursor += remaining;
            }
        }
        return s;
    }

    std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_acquire);
    }

    bool writeFile(const char* path) const {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        struct Header {
            char magic[4];
            std::uint32_t ver;
            std::uint32_t threads;
            std::uint32_t evSize;
        } h{{'B','T','R','C'}, 1u, static_cast<std::uint32_t>(buffers_.size()),
             static_cast<std::uint32_t>(sizeof(Event))};
        f.write(reinterpret_cast<const char*>(&h), sizeof(h));
        for (std::size_t t=0; t<buffers_.size(); ++t) {
            const Buffer& b = *buffers_[t];
            const std::size_t w = b.write.load(std::memory_order_acquire);
            const std::size_t n = (w < b.cap) ? w : b.cap;
            std::uint64_t cnt = static_cast<std::uint64_t>(n);
            f.write(reinterpret_cast<const char*>(&cnt), sizeof(cnt));
            if (n == 0) continue;
            std::size_t start = (w >= b.cap) ? (w % b.cap) : 0;
            std::size_t tail = b.cap - start;
            if (w < b.cap) tail = n;
            const std::size_t firstChunk = (tail > n) ? n : tail;
            f.write(reinterpret_cast<const char*>(b.base + start),
                    static_cast<std::streamsize>(firstChunk * sizeof(Event)));
            const std::size_t remaining = n - firstChunk;
            if (remaining)
                f.write(reinterpret_cast<const char*>(b.base),
                        static_cast<std::streamsize>(remaining * sizeof(Event)));
        }
        return true;
    }

private:
    struct Buffer {
        void*     mem   = nullptr;
        Event*    base  = nullptr;
        std::size_t cap = 0;
        std::atomic<std::size_t> write{0};
        std::atomic<std::uint64_t> dropped{0};

        Buffer() = default;
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        ~Buffer() { release(); }

        void allocate(std::size_t newCap) {
            release();
            if (newCap == 0)
                return;
            cap = newCap;
            mem = ::operator new(cap * sizeof(Event), std::align_val_t(64));
            base = static_cast<Event*>(mem);
            write.store(0, std::memory_order_relaxed);
            dropped.store(0, std::memory_order_relaxed);
        }

        void release() {
            if (mem) {
                ::operator delete(mem, std::align_val_t(64));
                mem = nullptr;
                base = nullptr;
                cap = 0;
                write.store(0, std::memory_order_relaxed);
            }
            dropped.store(0, std::memory_order_relaxed);
        }
    };

    inline void recordDrop(Buffer* buf) noexcept {
        buf->dropped.fetch_add(1, std::memory_order_relaxed);
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<std::unique_ptr<Buffer>> buffers_;
    bool enabled_ = false;
    std::size_t eventsPerThread_ = 0;
    std::atomic<std::uint64_t> dropped_{0};

    // TLS writer cursor
    static thread_local Buffer* tlsBuf_;
    static thread_local std::size_t tlsThreadIdx_;
};

inline thread_local typename Trace::Buffer* Trace::tlsBuf_ = nullptr;
inline thread_local std::size_t            Trace::tlsThreadIdx_ = ~std::size_t{0};

namespace detail {
inline std::atomic<Trace*> g_trace{nullptr};
} // namespace detail

inline void set_global_trace(Trace* trace) {
    detail::g_trace.store(trace, std::memory_order_release);
}

inline Trace* global_trace() {
    return detail::g_trace.load(std::memory_order_acquire);
}

inline void log_queue_push(std::uint32_t depth, std::uint64_t capacity = 0) {
    if (auto* trace = global_trace()) {
        trace->log(EV_QueuePush, depth, capacity);
    }
}

inline void log_queue_pop(std::uint32_t depth, std::uint64_t capacity = 0) {
    if (auto* trace = global_trace()) {
        trace->log(EV_QueuePop, depth, capacity);
    }
}

inline std::uint64_t encode_worker_meta(std::int32_t numaNode) noexcept {
    return static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(static_cast<std::int32_t>(numaNode)));
}

inline std::int32_t decode_worker_meta_node(std::uint64_t payload) noexcept {
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(payload & 0xFFFFFFFFu));
}

} // namespace bintrace

#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>
#include <cstring>
#include <memory>
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

enum : std::uint32_t {
    EV_PhaseBegin  = 0x01,
    EV_PhaseEnd    = 0x02,
    EV_ChunkStart  = 0x03,
    EV_ChunkDone   = 0x04,
    EV_BudgetLadder = 0x05,
};

static inline std::uint64_t rdtsc() noexcept {
    return HighResClock::now();
}

class Trace {
public:
    Trace() = default;

    void init(std::size_t threads, std::size_t eventsPerThread, bool enabled) {
        enabled_ = enabled;
        buffers_.clear();
        buffers_.resize(threads); // needs movable/default-constructible Buffer
        for (std::size_t i=0;i<threads;++i) {
            Buffer& b = buffers_[i];
            b.cap = eventsPerThread;
            b.mem = ::operator new(b.cap * sizeof(Event), std::align_val_t(64));
            b.base = static_cast<Event*>(b.mem);
            b.write.store(0, std::memory_order_relaxed);
        }
        tlsBuf_ = nullptr;
        tlsThreadIdx_ = ~std::size_t{0};
    }

    void shutdown() {
        for (auto& b : buffers_) {
            if (b.mem) {
                ::operator delete(b.mem, std::align_val_t(64));
                b.mem = nullptr; b.base = nullptr; b.cap = 0;
                b.write.store(0, std::memory_order_relaxed);
            }
        }
        buffers_.clear();
        tlsBuf_ = nullptr;
        tlsThreadIdx_ = ~std::size_t{0};
    }

    bool enabled() const noexcept { return enabled_; }

    void bindThread(std::size_t idx) {
        if (idx >= buffers_.size()) return;
        tlsBuf_ = &buffers_[idx];
        tlsThreadIdx_ = idx;
    }

    inline void log(std::uint32_t code, std::uint32_t a=0, std::uint64_t b=0) noexcept {
        if (!enabled_) return;
        Buffer* buf = tlsBuf_;
        if (!buf) return;
        const std::size_t cap = buf->cap;
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
            const Buffer& b = buffers_[t];
            const std::size_t w = b.write.load(std::memory_order_acquire);
            std::size_t n = (w < b.cap) ? w : b.cap;
            s.perThreadCount[t] = n;
            total += n;
        }
        s.events.resize(total);

        std::size_t cursor = 0;
        for (std::size_t t=0; t<buffers_.size(); ++t) {
            const Buffer& b = buffers_[t];
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
            const Buffer& b = buffers_[t];
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
        std::atomic<std::size_t> write;

        Buffer() : write(0) {}

        // non-copyable (atomic)
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        // movable (manual because atomic is non-movable)
        Buffer(Buffer&& o) noexcept
            : mem(o.mem), base(o.base), cap(o.cap), write(o.write.load(std::memory_order_relaxed)) {
            o.mem = nullptr; o.base = nullptr; o.cap = 0; o.write.store(0, std::memory_order_relaxed);
        }
        Buffer& operator=(Buffer&& o) noexcept {
            if (this != &o) {
                mem = o.mem; base = o.base; cap = o.cap;
                write.store(o.write.load(std::memory_order_relaxed), std::memory_order_relaxed);
                o.mem = nullptr; o.base = nullptr; o.cap = 0; o.write.store(0, std::memory_order_relaxed);
            }
            return *this;
        }
    };

    std::vector<Buffer> buffers_;
    bool enabled_ = false;

    // TLS writer cursor
    static thread_local Buffer* tlsBuf_;
    static thread_local std::size_t tlsThreadIdx_;
};

inline thread_local typename Trace::Buffer* Trace::tlsBuf_ = nullptr;
inline thread_local std::size_t            Trace::tlsThreadIdx_ = ~std::size_t{0};

} // namespace bintrace

#pragma once

#include <deque>
#include <mutex>
#include <cstddef>

//
// Simple bounded multi-producer/multi-consumer queue.
// The original project used a lock-free implementation based on
// Dmitry Vyukov's algorithm.  During recent merges the file became
// corrupted which broke the build.  This version provides a minimal,
// thread-safe implementation using a std::deque protected by a mutex.
// It preserves the public API used by WorkerPool while trading the
// lock-free property for simplicity and correctness.
//
template <typename T>
class BoundedMPMCQueue {
public:
    explicit BoundedMPMCQueue(std::size_t capacityPow2)
        : capacity_(nextPow2(capacityPow2 ? capacityPow2 : 1)) {}

    // Non-blocking push; returns false if the queue is full.
    bool try_push(T&& v) {
        std::lock_guard<std::mutex> lock(m_);
        if (q_.size() >= capacity_) {
            return false;
        }
        q_.emplace_back(std::move(v));
        return true;
    }

    // Non-blocking pop; returns false if the queue is empty.
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(m_);
        if (q_.empty()) {
            return false;
        }
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    std::size_t capacity() const { return capacity_; }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_);
        return q_.size();
    }

    bool empty() const { return size() == 0; }

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

    const std::size_t capacity_;
    mutable std::mutex m_;
    std::deque<T> q_;
};


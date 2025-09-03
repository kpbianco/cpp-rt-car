#pragma once
#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>
#include <chrono>
#include "job_queue.hpp"

// Simple persistent worker pool built on the lock-free queue.
// Jobs are std::function<void()>.
//
// Usage:
//   WorkerPool pool(threads, 1024);
//   pool.enqueue([]{ ... });
//   pool.drain();   // wait until all queued jobs finish
//   pool.stop();    // join threads (optional; destructor does this too)
//
class WorkerPool {
public:
    using Job = std::function<void()>;

    WorkerPool(std::size_t numThreads, std::size_t queueSizePow2 = 1024, bool verbose = false)
    : queue_(queueSizePow2), verbose_(verbose)
    {
        stopping_.store(false, std::memory_order_relaxed);
        active_.store(0, std::memory_order_relaxed);
        threads_.reserve(numThreads);
        for (std::size_t i = 0; i < numThreads; ++i) {
            threads_.emplace_back([this] { this->workerLoop(); });
        }
    }

    ~WorkerPool() { stop(); }

    // Busy-wait enqueue (lock-free); returns after the job is on the queue.
    void enqueue(Job j) {
        while (!queue_.try_push(std::move(j))) {
            // Backoff to reduce contention if the queue is temporarily full
            std::this_thread::yield();
        }
    }

    // Non-blocking variant; returns false if full.
    bool try_enqueue(Job j) {
        return queue_.try_push(std::move(j));
    }

    // Wait until queue is empty and all workers are idle.
    void drain() {
        using namespace std::chrono_literals;
        for (;;) {
            if (queue_.empty() && active_.load(std::memory_order_acquire) == 0) break;
            std::this_thread::sleep_for(50us);
        }
    }

    // Stop workers after draining; safe to call multiple times.
    void stop() {
        bool expected = false;
        if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            // already stopping/stopped
        }
        // Give workers a chance to exit when idle; also drain any stragglers
        drain();
        for (auto &t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
    }

    std::size_t approx_queue_size() const { return queue_.size(); }
    std::size_t thread_count()       const { return threads_.size(); }

private:
    void workerLoop() {
        for (;;) {
            Job job;
            if (queue_.try_pop(job)) {
                active_.fetch_add(1, std::memory_order_acq_rel);
                // Guard against empty std::function (shouldn't happen, but be safe)
                if (job) job(); else if (verbose_) {/*no-op*/ }
                active_.fetch_sub(1, std::memory_order_acq_rel);
                continue;
            }
            if (stopping_.load(std::memory_order_acquire)) {
                // If asked to stop and nothing to do, exit.
                if (queue_.empty() && active_.load(std::memory_order_acquire) == 0)
                    break;
            }
            std::this_thread::yield();
        }
    }

    BoundedMPMCQueue<Job>  queue_;
    std::vector<std::thread> threads_;
    std::atomic<bool>        stopping_{false};
    std::atomic<std::size_t> active_{0};
    bool                     verbose_{false};
};

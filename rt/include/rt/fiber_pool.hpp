#pragma once

#include <coroutine>
#include <deque>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <utility>

#include <simcore/hal.hpp>

namespace rt {

// Coroutine task type used by FiberPool
struct Task {
    struct promise_type {
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;
    handle_type h;
    explicit Task(handle_type handle) : h(handle) {}
    Task(Task&& other) noexcept : h(std::exchange(other.h, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (h)
                h.destroy();
            h = std::exchange(other.h, {});
        }
        return *this;
    }
    ~Task() {
        if (h)
            h.destroy();
    }
};

class FiberPool {
public:
    explicit FiberPool(std::size_t threads = std::thread::hardware_concurrency()) {
        start(threads);
    }

    ~FiberPool() { stop(); }

    void spawn(Task task) {
        if (!task.h)
            return;
        pending_.fetch_add(1, std::memory_order_acq_rel);
        schedule(task.h);
        task.h = {};
    }

    void drain() {
        using namespace std::chrono_literals;
        while (pending_.load(std::memory_order_acquire) > 0) {
            std::this_thread::sleep_for(1ms);
        }
    }

    void stop() {
        bool expected = false;
        if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return; // already stopping
        {
            std::lock_guard<std::mutex> lock(readyMutex_);
            readyCv_.notify_all();
        }
        if (poller_.joinable())
            poller_.join();
        for (auto& t : workers_)
            if (t.joinable())
                t.join();
        workers_.clear();
    }

    // Awaiter for hal::Fence
    struct FenceAwaiter {
        simcore::hal::Fence& fence;
        FiberPool& pool;
        bool await_ready() const noexcept { return fence.is_signaled(); }
        void await_suspend(std::coroutine_handle<> h) const {
            pool.add_waiter(&fence, h);
        }
        void await_resume() const noexcept {}
    };

private:
    struct WaitItem {
        simcore::hal::Fence* fence;
        std::coroutine_handle<> handle;
    };

    void start(std::size_t threads) {
        stopping_.store(false, std::memory_order_relaxed);
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
        poller_ = std::thread([this] { poller_loop(); });
    }

    void schedule(std::coroutine_handle<> h) {
        {
            std::lock_guard<std::mutex> lock(readyMutex_);
            ready_.push_back(h);
        }
        readyCv_.notify_one();
    }

    void add_waiter(simcore::hal::Fence* f, std::coroutine_handle<> h) {
        std::lock_guard<std::mutex> lock(waitMutex_);
        waiters_.push_back(WaitItem{f, h});
    }

    void worker_loop() {
        for (;;) {
            std::coroutine_handle<> h;
            {
                std::unique_lock<std::mutex> lock(readyMutex_);
                readyCv_.wait(lock, [this] { return stopping_.load(std::memory_order_acquire) || !ready_.empty(); });
                if (stopping_.load(std::memory_order_acquire) && ready_.empty())
                    break;
                h = ready_.front();
                ready_.pop_front();
            }
            h.resume();
            if (h.done()) {
                h.destroy();
                pending_.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    }

    void poller_loop() {
        using namespace std::chrono_literals;
        while (!stopping_.load(std::memory_order_acquire)) {
            std::vector<std::coroutine_handle<>> ready;
            {
                std::lock_guard<std::mutex> lock(waitMutex_);
                for (auto it = waiters_.begin(); it != waiters_.end();) {
                    if (it->fence->is_signaled()) {
                        ready.push_back(it->handle);
                        it = waiters_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (auto h : ready)
                schedule(h);
            std::this_thread::sleep_for(1ms);
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::coroutine_handle<>> ready_;
    std::mutex readyMutex_;
    std::condition_variable readyCv_;
    std::atomic<bool> stopping_{false};

    std::mutex waitMutex_;
    std::vector<WaitItem> waiters_;
    std::thread poller_;

    std::atomic<std::size_t> pending_{0};
};

} // namespace rt


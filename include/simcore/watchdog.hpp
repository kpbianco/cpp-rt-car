#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include "debug.hpp"

class Watchdog {
public:
    using Callback = std::function<void()>;
    Watchdog(std::chrono::milliseconds timeout, Callback cb)
        : timeout_(timeout), cb_(std::move(cb)), stop_(false)
    {
        last_ = std::chrono::steady_clock::now();
        simcore::debug::assert_thread_creation_allowed();
        worker_ = std::thread([this]{ run(); });
    }
    ~Watchdog()
    {
        stop_ = true;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    void touch()
    {
        std::lock_guard<std::mutex> lk(m_);
        last_ = std::chrono::steady_clock::now();
        cv_.notify_all();
    }
private:
    void run()
    {
        std::unique_lock<std::mutex> lk(m_);
        while (!stop_) {
            if (cv_.wait_until(lk, last_ + timeout_, [this]{ return stop_.load(); })) break;
            if (std::chrono::steady_clock::now() - last_ >= timeout_) {
                lk.unlock();
                cb_();
                lk.lock();
                last_ = std::chrono::steady_clock::now();
            }
        }
    }
    std::chrono::milliseconds timeout_;
    Callback cb_;
    std::atomic<bool> stop_;
    std::chrono::steady_clock::time_point last_;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
};

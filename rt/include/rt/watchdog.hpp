#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace rt {

class Watchdog {
public:
    using Callback = std::function<void()>;

    Watchdog(std::chrono::milliseconds budget,
             std::chrono::milliseconds limp_budget,
             Callback limp_cb)
        : budget_(budget),
          limp_budget_(limp_budget),
          limp_cb_(std::move(limp_cb)),
          stop_(false) {
        last_ = std::chrono::steady_clock::now();
        worker_ = std::thread([this] { run(); });
    }

    ~Watchdog() {
        stop_ = true;
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void touch() {
        std::lock_guard<std::mutex> lk(m_);
        last_ = std::chrono::steady_clock::now();
        cv_.notify_all();
    }

    bool limp() const noexcept { return limp_.load(); }

private:
    void run() {
        std::unique_lock<std::mutex> lk(m_);
        while (!stop_) {
            auto next = last_ + (limp_.load() ? limp_budget_ : budget_);
            if (cv_.wait_until(lk, next, [this] { return stop_.load(); })) {
                break;
            }
            if (std::chrono::steady_clock::now() >= next) {
                limp_ = true;
                lk.unlock();
                if (limp_cb_) {
                    limp_cb_();
                }
                lk.lock();
                last_ = std::chrono::steady_clock::now();
            }
        }
    }

    std::chrono::milliseconds budget_;
    std::chrono::milliseconds limp_budget_;
    Callback limp_cb_;
    std::atomic<bool> limp_{false};
    std::atomic<bool> stop_;
    std::chrono::steady_clock::time_point last_;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
};

} // namespace rt


#pragma once

#include <vector>
#include <thread>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <initializer_list>

class WorkerPool {
public:
    using Job = std::function<void()>;

    // Single‑argument ctor (tests call WorkerPool pool(4);)
    explicit WorkerPool(std::size_t numThreads)
      : shutdown_(false)
    {
        start(numThreads);
    }

    // Two‑arg overload for backwards compatibility
    WorkerPool(std::size_t numThreads, std::size_t /*queueSize*/)
      : shutdown_(false)
    {
        start(numThreads);
    }

    ~WorkerPool() {
        shutdown();
    }

    // Enqueue a single job
    void enqueue(Job j) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            jobQueue_.push(std::move(j));
        }
        condVar_.notify_one();
    }

    // Enqueue via initializer_list so that pool.enqueue({[](){…}}) works
    void enqueue(std::initializer_list<Job> jobs) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto &j : jobs) {
                jobQueue_.push(j);
            }
        }
        condVar_.notify_all();
    }

    // Gracefully shut down: wake all threads, drain nothing more, join
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            shutdown_ = true;
        }
        condVar_.notify_all();
        for (auto &t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
    }

private:
    void start(std::size_t numThreads) {
        threads_.reserve(numThreads);
        for (std::size_t i = 0; i < numThreads; ++i) {
            threads_.emplace_back([this] {
                for (;;) {
                    Job job;
                    {
                        std::unique_lock<std::mutex> lk(mutex_);
                        condVar_.wait(lk, [this] {
                            return shutdown_ || !jobQueue_.empty();
                        });
                        if (shutdown_ && jobQueue_.empty())
                            return;
                        job = std::move(jobQueue_.front());
                        jobQueue_.pop();
                    }
                    // invoke outside lock
                    job();
                }
            });
        }
    }

    std::vector<std::thread>    threads_;
    std::queue<Job>             jobQueue_;
    std::mutex                  mutex_;
    std::condition_variable     condVar_;
    bool                        shutdown_;
};

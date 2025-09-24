#include <rt/scheduler.hpp>

#include <algorithm>

#include <rt/arch.hpp>

namespace rt {

Scheduler::Scheduler(std::size_t threads) {
  if (threads == 0)
    threads = 1;
  workers_.reserve(threads);
  for (std::size_t i = 0; i < threads; ++i) {
    workers_.emplace_back(std::make_unique<Worker>());
  }
}

Scheduler::~Scheduler() {
  // Ensure all threads joined
  for (auto &w : workers_) {
    if (w->thread.joinable())
      w->thread.join();
  }
}

void Scheduler::addTask(Task t) {
  auto idx = next_.fetch_add(1, std::memory_order_relaxed) % workers_.size();
  {
    auto &w = workers_[idx];
    std::lock_guard<std::mutex> lock(w->mutex);
    w->pending.push_back(std::move(t));
    remaining_.fetch_add(1, std::memory_order_relaxed);
  }
}

bool Scheduler::stealTask(std::size_t thief, Task &out) {
  for (std::size_t i = 0; i < workers_.size(); ++i) {
    if (i == thief)
      continue;
    auto &w = workers_[i];
    std::lock_guard<std::mutex> lock(w->mutex);
    if (!w->queue.empty()) {
      out = std::move(w->queue.back());
      w->queue.pop_back();
      return true;
    }
  }
  return false;
}

void Scheduler::workerLoop(std::size_t index) {
  auto &self = *workers_[index];
  for (;;) {
    Task task;
    {
      std::lock_guard<std::mutex> lock(self.mutex);
      if (!self.queue.empty()) {
        task = pop_next_with_aging(self.queue);
      }
    }
    if (!task.fn) {
      if (!stealTask(index, task)) {
        if (remaining_.load(std::memory_order_acquire) == 0)
          break; // all tasks done
        rt::cpu_relax();
        continue;
      }
    }
    task.fn();
    remaining_.fetch_sub(1, std::memory_order_relaxed);
  }
}

void Scheduler::run(Policy policy) {
  // Prepare queues based on policy
  for (auto &wptr : workers_) {
    auto &w = *wptr;
    std::lock_guard<std::mutex> lock(w.mutex);
    auto &p = w.pending;
    auto cmpEDF = [](const Task &a, const Task &b) {
      return a.deadline < b.deadline;
    };
    auto cmpFixed = [](const Task &a, const Task &b) {
      return a.period < b.period;
    };
    if (policy == Policy::EDF)
      std::sort(p.begin(), p.end(), cmpEDF);
    else
      std::sort(p.begin(), p.end(), cmpFixed);
    for (auto &t : p)
      w.queue.push_back(std::move(t));
    p.clear();
  }

  // Launch workers
  for (std::size_t i = 0; i < workers_.size(); ++i) {
    auto &w = workers_[i];
    w->thread = std::thread([this, i] { workerLoop(i); });
  }
  // Join workers
  for (auto &w : workers_) {
    if (w->thread.joinable())
      w->thread.join();
  }
}

} // namespace rt

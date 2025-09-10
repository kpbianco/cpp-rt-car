#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory_resource>
#include <mutex>
#include <thread>
#include <vector>

namespace rt {

// Scheduling policies supported by Scheduler
enum class Policy { EDF, FixedFrame };

// Task representation used by Scheduler
struct Task {
  std::function<void()> fn;                       // Task body
  std::chrono::steady_clock::time_point deadline; // Absolute deadline
  std::chrono::milliseconds period{0};            // Period for fixed-frame
  int priority{0};                                // Base priority
  int age{0};                                     // Aging counter
};

// Utility function used by workers to select the next task while applying
// priority aging. The task with the highest (priority + age) is removed from
// the queue and returned. Remaining tasks have their age incremented to avoid
// starvation. This function is exposed for unit testing as well.
template <class Alloc> Task pop_next_with_aging(std::deque<Task, Alloc> &q) {
  auto best = q.begin();
  for (auto it = q.begin(); it != q.end(); ++it) {
    it->age++;
    if (it->priority + it->age > best->priority + best->age)
      best = it;
  }
  Task out = std::move(*best);
  q.erase(best);
  return out;
}

// Simple work-stealing scheduler supporting EDF vs fixed-frame ordering,
// priority based scheduling with aging, and per-thread local memory arenas.
class Scheduler {
public:
  explicit Scheduler(std::size_t threads = std::thread::hardware_concurrency());
  ~Scheduler();

  // Non-copyable
  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;

  // Add a task to the scheduler. Tasks can be added from multiple threads.
  void addTask(Task t);

  // Execute all queued tasks using the given scheduling policy.
  void run(Policy policy);

private:
  struct Worker {
    std::pmr::monotonic_buffer_resource arena; // per-thread arena
    std::pmr::vector<Task> pending;            // tasks awaiting ordering
    std::pmr::deque<Task> queue;               // ready tasks
    std::mutex mutex;                          // protects queue during steals
    std::thread thread;                        // worker thread

    Worker() : arena(), pending(&arena), queue(&arena) {}
  };

  std::vector<std::unique_ptr<Worker>> workers_;
  std::atomic<std::size_t> next_{0};
  std::atomic<std::size_t> remaining_{0};

  void workerLoop(std::size_t index);
  bool stealTask(std::size_t thief, Task &out);
};

} // namespace rt

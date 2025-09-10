#include <gtest/gtest.h>
#include <rt/scheduler.hpp>

#include <chrono>
#include <deque>
#include <vector>

using namespace std::chrono_literals;

TEST(Scheduler, EDFvsFixedFrame) {
  std::vector<char> order;
  // EDF policy
  {
    rt::Scheduler sched(1);
    sched.addTask(rt::Scheduler::Task{[&] { order.push_back('A'); },
                                      std::chrono::steady_clock::now() + 10ms,
                                      5ms, 0});
    sched.addTask(rt::Scheduler::Task{[&] { order.push_back('B'); },
                                      std::chrono::steady_clock::now() + 5ms,
                                      20ms, 0});
    sched.run(rt::Policy::EDF);
    EXPECT_EQ(order, (std::vector<char>{'B', 'A'}));
  }
  order.clear();
  // Fixed frame policy
  {
    rt::Scheduler sched(1);
    sched.addTask(rt::Scheduler::Task{[&] { order.push_back('A'); },
                                      std::chrono::steady_clock::now() + 10ms,
                                      5ms, 0});
    sched.addTask(rt::Scheduler::Task{[&] { order.push_back('B'); },
                                      std::chrono::steady_clock::now() + 5ms,
                                      20ms, 0});
    sched.run(rt::Policy::FixedFrame);
    EXPECT_EQ(order, (std::vector<char>{'A', 'B'}));
  }
}

TEST(Scheduler, AgingPreventsStarvation) {
  std::deque<rt::Scheduler::Task> q;
  // Low priority task
  q.push_back(rt::Scheduler::Task{[] {}, std::chrono::steady_clock::now(),
                                  1ms, 0});
  // High priority task that we will repeatedly reinsert
  q.push_back(rt::Scheduler::Task{[] {}, std::chrono::steady_clock::now(),
                                  1ms, 5});

  bool low_seen = false;
  for (int i = 0; i < 10; ++i) {
    rt::Scheduler::Task t = rt::Scheduler::pop_next_with_aging(q);
    if (t.priority == 0) {
      low_seen = true;
      break;
    }
    // Reinsert another high priority task to keep the queue busy
    q.push_back(rt::Scheduler::Task{[] {}, std::chrono::steady_clock::now(),
                                    1ms, 5});
  }
  EXPECT_TRUE(low_seen);
}

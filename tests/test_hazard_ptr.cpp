#include "simcore/hazard_ptr.hpp"
#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace simcore;

struct Node {
  int value;
  std::atomic<Node *> next;
  Node(int v) : value(v), next(nullptr) {}
};

class LockFreeStack {
  std::atomic<Node *> head{nullptr};

public:
  void push(int v) {
    Node *n = new Node(v);
    Node *expected = head.load(std::memory_order_relaxed);
    do {
      n->next.store(expected, std::memory_order_relaxed);
    } while (!head.compare_exchange_weak(expected, n, std::memory_order_release,
                                         std::memory_order_relaxed));
  }

  bool pop(int &out) {
    HazardGuard guard;
    Node *h = nullptr;
    Node *next = nullptr;
    h = guard.protect(head);
    while (h) {
      next = h->next.load(std::memory_order_relaxed);
      if (head.compare_exchange_weak(h, next,
                                     std::memory_order_seq_cst,
                                     std::memory_order_seq_cst)) {
        out = h->value;
        guard.clear();
        retire(h, [](void *p) { delete static_cast<Node *>(p); });
        return true;
      }
      h = guard.protect(head);
    }
    return false;
  }
};

static std::size_t exercise_stack(int count) {
  LockFreeStack st;
  std::thread t1([&] {
    for (int i = 0; i < count; ++i)
      st.push(i);
  });
  std::thread t2([&] {
    for (int i = 0; i < count; ++i)
      st.push(i);
  });
  t1.join();
  t2.join();
  std::vector<int> vals1, vals2;
  vals1.reserve(count);
  vals2.reserve(count);
  std::thread c1([&] {
    int x;
    while (st.pop(x))
      vals1.push_back(x);
  });
  std::thread c2([&] {
    int x;
    while (st.pop(x))
      vals2.push_back(x);
  });
  c1.join();
  c2.join();
  scan(); // reclaim any remaining
  return vals1.size() + vals2.size();
}

TEST(HazardPointer, StackCorrectness) {
  constexpr int N = 1000;
  for (int iteration = 0; iteration < 32; ++iteration) {
    EXPECT_EQ(exercise_stack(N), 2u * N);
  }
}

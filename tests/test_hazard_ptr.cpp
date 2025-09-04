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
    Node *h = head.load(std::memory_order_acquire);
    while (h) {
      guard.protect(head);
      if (head.compare_exchange_weak(h, h->next.load(std::memory_order_relaxed),
                                     std::memory_order_acquire,
                                     std::memory_order_relaxed)) {
        out = h->value;
        retire(h, [](void *p) { delete static_cast<Node *>(p); });
        return true;
      }
    }
    return false;
  }
};

TEST(HazardPointer, StackCorrectness) {
  LockFreeStack st;
  const int N = 1000;
  std::thread t1([&] {
    for (int i = 0; i < N; ++i)
      st.push(i);
  });
  std::thread t2([&] {
    for (int i = 0; i < N; ++i)
      st.push(i);
  });
  t1.join();
  t2.join();
  std::vector<int> vals;
  vals.reserve(2 * N);
  std::thread c1([&] {
    int x;
    while (st.pop(x))
      vals.push_back(x);
  });
  std::thread c2([&] {
    int x;
    while (st.pop(x))
      vals.push_back(x);
  });
  c1.join();
  c2.join();
  scan(); // reclaim any remaining
  EXPECT_EQ(vals.size(), 2u * N);
}

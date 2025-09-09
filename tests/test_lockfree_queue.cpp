#include "simcore/lockfree_queue.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace simcore;

TEST(LockFreeQueue, MultiProducerConsumer) {
  LockFreeQueue<int> q;
  const int N = 1000;
  std::thread p1([&] {
    for (int i = 0; i < N; ++i)
      q.push(i);
  });
  std::thread p2([&] {
    for (int i = 0; i < N; ++i)
      q.push(i);
  });
  p1.join();
  p2.join();
  std::vector<int> out1, out2;
  std::thread c1([&] {
    int v;
    while (q.pop(v))
      out1.push_back(v);
  });
  std::thread c2([&] {
    int v;
    while (q.pop(v))
      out2.push_back(v);
  });
  c1.join();
  c2.join();
  scan();
  EXPECT_EQ(out1.size() + out2.size(), static_cast<size_t>(2 * N));
}

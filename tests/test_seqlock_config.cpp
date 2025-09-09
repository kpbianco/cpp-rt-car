#include "simcore/seqlock.hpp"
#include <atomic>
#include <gtest/gtest.h>
#include <thread>

using namespace simcore;

TEST(SeqLock, ConsistentReads) {
  struct State {
    int a;
    int b;
  };
  SeqLock<State> lock({0, 0});
  std::atomic<bool> run{true};
  std::thread writer([&]() {
    for (int i = 1; i <= 10000; ++i) {
      lock.write({i, i});
    }
    run.store(false);
  });
  std::thread reader([&]() {
    while (run.load()) {
      State s = lock.read();
      EXPECT_EQ(s.a, s.b);
    }
  });
  writer.join();
  run.store(false);
  reader.join();
}

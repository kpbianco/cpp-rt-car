#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <simcore/rt_memory.hpp>
#include <thread>
#include <vector>

#ifdef SIM_USE_NUMA
#include <numa.h>

TEST(NumaPool, FirstTouchBinding) {
  if (numa_available() < 0 || numa_num_configured_nodes() < 2) {
    GTEST_SKIP() << "NUMA not available";
  }
  std::vector<int> nodes = {0, 1};
  rt::FrameArenaPool pool(2, 1u << 12, 64, nodes);
  std::vector<void *> ptrs(2);
  std::thread t0([&]() {
    pool.bindCurrentThread(0);
    auto &a = pool.tls();
    ptrs[0] = a.allocate(4096);
    std::memset(ptrs[0], 1, 4096);
  });
  std::thread t1([&]() {
    pool.bindCurrentThread(1);
    auto &a = pool.tls();
    ptrs[1] = a.allocate(4096);
    std::memset(ptrs[1], 2, 4096);
  });
  t0.join();
  t1.join();

  void *addrs[2] = {ptrs[0], ptrs[1]};
  int status[2];
  numa_move_pages(0, 2, addrs, nullptr, status, 0);
  EXPECT_EQ(status[0], 0);
  EXPECT_EQ(status[1], 1);
}

static double touch(char *p, size_t bytes) {
  volatile char sum = 0;
  for (size_t i = 0; i < bytes; i += 64)
    sum += p[i];
  return sum;
}

TEST(NumaPool, ABABvsAABBLatency) {
  if (numa_available() < 0 || numa_num_configured_nodes() < 2) {
    GTEST_SKIP() << "NUMA not available";
  }
  constexpr size_t bytes = 1u << 18;
  rt::FrameArena a0(bytes, 64, 0);
  rt::FrameArena a1(bytes, 64, 1);
  char *p0 = static_cast<char *>(a0.allocate(bytes));
  char *p1 = static_cast<char *>(a1.allocate(bytes));
  std::memset(p0, 0, bytes);
  std::memset(p1, 0, bytes);

  auto measure = [&](char *a, char *b) {
    auto start = std::chrono::high_resolution_clock::now();
    touch(a, bytes);
    touch(b, bytes);
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double>(end - start).count();
  };

  double abab = measure(p0, p1) + measure(p1, p0);
  double aabb = measure(p0, p0) + measure(p1, p1);
  EXPECT_LT(aabb, abab);
}
#else
TEST(NumaPool, Skipped) { GTEST_SKIP() << "SIM_USE_NUMA not enabled"; }
#endif

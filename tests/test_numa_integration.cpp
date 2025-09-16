#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>

#ifdef SIM_USE_NUMA
#include <numa.h>
#include <sched.h>

namespace {
struct ArenaCapture {
  void push(void *addr, int node) {
    std::lock_guard<std::mutex> lock(mutex);
    addresses.push_back(addr);
    nodes.push_back(node);
  }

  std::mutex mutex;
  std::vector<void *> addresses;
  std::vector<int> nodes;
};
} // namespace

TEST(NumaIntegration, WorkerArenasAreNodeLocal) {
  if (numa_available() < 0 || numa_num_configured_nodes() < 2) {
    GTEST_SKIP() << "NUMA not available";
  }
  if (std::thread::hardware_concurrency() < 2) {
    GTEST_SKIP() << "Insufficient hardware concurrency";
  }

  SimCore::Settings cfg;
  cfg.threads = 2;
  cfg.pinThreads = true;
  cfg.compactNUMA = true;
  cfg.maxFrames = 1;
  cfg.autoTuneChunks = false;
  cfg.chunkSize = 1;
  cfg.arenaPerThreadBytes = 1u << 12;

  SimCore sim(cfg);
  std::size_t phase = sim.addPhase("numa", cfg.threads * 8);

  ArenaCapture capture;
  sim.addParallelRangeTask(
      phase, [&sim, &capture](std::size_t, std::size_t, std::int64_t,
                              SimCore::Seconds) {
        static thread_local bool recorded = false;
        if (recorded)
          return;
        recorded = true;
        void *ptr = sim.frameArena().allocate(4096);
        std::memset(ptr, 1, 4096);
        int cpu = sched_getcpu();
        int node = (cpu >= 0) ? numa_node_of_cpu(cpu) : -1;
        capture.push(ptr, node);
      });

  sim.run();

  ASSERT_FALSE(capture.addresses.empty());
  ASSERT_GE(capture.addresses.size(), cfg.threads);
  std::vector<void *> pages = capture.addresses;
  std::vector<int> status(pages.size());
  numa_move_pages(0, static_cast<unsigned long>(pages.size()), pages.data(),
                  nullptr, status.data(), 0);

  for (std::size_t i = 0; i < pages.size(); ++i) {
    ASSERT_GE(capture.nodes[i], 0);
    EXPECT_EQ(status[i], capture.nodes[i]);
  }
}
#else
TEST(NumaIntegration, Skipped) {
  GTEST_SKIP() << "SIM_USE_NUMA not enabled";
}
#endif


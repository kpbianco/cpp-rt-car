#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/worker_pool.hpp>

TEST(RTPipeline, QueueMetricsAndNoThreadsInSrc) {
  WorkerPool pool(1);
  SimCore::Settings s;
  s.maxFrames = 1;
  s.threads = 0; // disable internal threads so worker pool runs phases
  SimCore sim(s);
  sim.setWorkerPool(&pool);

  auto p1 = sim.addPhase("p1");
  auto p2 = sim.addPhase("p2");
  auto sleeper = [](int64_t, SimCore::Seconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  };
  sim.addSerialSubsystem(p1, sleeper);
  sim.addSerialSubsystem(p2, sleeper);

  sim.run();
  pool.stop();

  auto stats = pool.stats();
  EXPECT_GT(stats.maxQueueDepth, 0u);
  EXPECT_GT(stats.steals, 0u);

  bool found = false;
  std::filesystem::path srcDir{PROJECT_SOURCE_DIR};
  srcDir /= "src";
  for (auto &entry : std::filesystem::recursive_directory_iterator(srcDir)) {
    if (!entry.is_regular_file())
      continue;
    std::ifstream in(entry.path());
    std::string line;
    while (std::getline(in, line)) {
      if (line.find("std::thread") != std::string::npos) {
        found = true;
        break;
      }
    }
    if (found)
      break;
  }
  EXPECT_FALSE(found);
}


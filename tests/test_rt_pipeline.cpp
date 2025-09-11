#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/worker_pool.hpp>

TEST(RTPipeline, QueueMetricsAndNoThreadsInSrc) {
  WorkerPool pool(2);
  SimCore::Settings s;
  s.maxFrames = 1;
  SimCore sim(s);
  sim.setWorkerPool(&pool);
  auto p = sim.addPhase("p");
  sim.addSerialSubsystem(p, [](int64_t, SimCore::Seconds) {});
  sim.run();
  pool.drain();

  auto stats = pool.stats();
  EXPECT_GT(stats.maxQueueDepth, 0u);
  EXPECT_GT(stats.steals, 0u);

  bool found = false;
  for (auto &entry : std::filesystem::recursive_directory_iterator("src")) {
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


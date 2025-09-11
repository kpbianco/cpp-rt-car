#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/worker_pool.hpp>
#include <string>
#include <thread>

// Ensure the worker pool backs all phase work and emits metrics.
TEST(RtPipeline, PoolMetricsPopulated) {
  SimCore::Settings s;
  s.hz = 60.0;
  s.maxFrames = 1;
  s.threads = 2;
  s.autoTuneChunks = false;
  s.chunkSize = 1;
  s.driftLogInterval = 0;

  WorkerPool pool(2);
  SimCore sim(s);
  sim.setWorkerPool(&pool);

  for (int i = 0; i < 8; ++i) {
    auto p = sim.addPhase("p" + std::to_string(i));
    sim.addSerialSubsystem(p, [](std::int64_t, SimCore::Seconds) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
  }

  sim.run();

  EXPECT_GT(pool.max_queue_depth(), 0u);
  EXPECT_GT(pool.steals(), 0u);
}

// Lint to ensure phases are not spawning raw threads.
TEST(RtPipeline, NoRawThreadSpawns) {
  namespace fs = std::filesystem;
  std::size_t count = 0;
  for (const auto &ent : fs::recursive_directory_iterator("src")) {
    if (!ent.is_regular_file())
      continue;
    std::ifstream in(ent.path());
    std::string line;
    while (std::getline(in, line)) {
      if (line.find("std::thread") != std::string::npos) {
        ++count;
        break;
      }
    }
  }
  EXPECT_EQ(count, 0u);
}

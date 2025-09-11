#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/worker_pool.hpp>

static void busy_spin(int micros) {
  using Clock = std::chrono::steady_clock;
  auto start = Clock::now();
  while (std::chrono::duration_cast<std::chrono::microseconds>(
             Clock::now() - start)
             .count() < micros) {
    std::atomic_signal_fence(std::memory_order_acq_rel);
  }
}

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

TEST(RTPipeline, WatchdogTripRecordedAndContinues) {
  WorkerPool pool(1);
  SimCore::Settings s;
  s.maxFrames = 2;
  s.threads = 0;
  SimCore sim(s);
  sim.setWorkerPool(&pool);

  auto p = sim.addPhase("stall");
  auto stall = [](int64_t, SimCore::Seconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  };
  sim.addSerialSubsystem(p, stall);

  sim.run();
  pool.stop();

  EXPECT_GT(sim.watchdogTrips(), 0);
  EXPECT_GE(sim.frame(), 2);
}

TEST(RTPipeline, RateGovernorDropsVisualsToMeetBudget) {
  WorkerPool pool(1);
  SimCore::Settings s;
  s.maxFrames = 50;
  s.threads = 0;
  s.hz = 200;
  SimCore sim(s);
  sim.setWorkerPool(&pool);

  auto visuals = sim.addPhase("visuals");
  auto physics = sim.addPhase("physics");

  sim.addSerialSubsystem(visuals, [&](int64_t, SimCore::Seconds) {
    if (sim.visualizersEnabled())
      busy_spin(10000);
  });
  sim.addSerialSubsystem(physics, [&](int64_t, SimCore::Seconds) {
    busy_spin(2000);
  });

  std::vector<double> frames;
  sim.setBudgetCallback(
      [&](const SimCore::BudgetSample &b) { frames.push_back(b.computeMs); });

  sim.run();
  pool.stop();

  EXPECT_GT(sim.visualsDropped(), 0);
  std::sort(frames.begin(), frames.end());
  std::size_t idx = static_cast<std::size_t>(std::floor(
      static_cast<double>(frames.size()) * 0.99));
  if (idx > 0)
    --idx;
  if (idx >= frames.size())
    idx = frames.size() - 1;
  double p99 = frames[idx];
  EXPECT_LE(p99, (1000.0 / s.hz) * 1.05);
}


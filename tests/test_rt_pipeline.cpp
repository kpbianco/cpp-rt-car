#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/worker_pool.hpp>
#include <gpu/frame_graph.hpp>
#include "tools/trace_export.hpp"

static void busy_for(double ms) {
  using Clock = std::chrono::steady_clock;
  auto start = Clock::now();
  auto target = std::chrono::duration<double, std::milli>(ms);
  while (Clock::now() - start < target)
    std::atomic_signal_fence(std::memory_order_acq_rel);
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
  EXPECT_GT(stats.totalSteals, 0u);

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

TEST(RTPipeline, RateGovernorClampsFrameBudget) {
  WorkerPool pool(1);
  SimCore::Settings s;
  s.hz = 120.0;
  s.maxFrames = 120;
  s.threads = 0;
  s.budgetMonitor = false;
  s.bintraceEnable = true;
  s.bintraceEventsPerThread = 1u << 10;
  s.rateGovernorTargetUtil = 0.9;
  s.rateGovernorHysteresis = 0.05;
  SimCore sim(s);
  sim.setWorkerPool(&pool);

  std::vector<double> samples;
  sim.setBudgetCallback([
    &samples
  ](const SimCore::BudgetSample &sample) { samples.push_back(sample.computeMs); });

  auto p = sim.addPhase("governed");
  sim.addSerialSubsystem(p, [&sim](std::int64_t, SimCore::Seconds) {
    if (sim.visualizersEnabled())
      busy_for(12.0);
    else
      busy_for(6.0);
  });

  sim.run();
  pool.stop();

  EXPECT_GE(sim.rungActivations(1), 1u);
  EXPECT_GE(sim.governorRungCount(1), 1u);

  ASSERT_FALSE(samples.empty());
  auto sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  std::size_t index = static_cast<std::size_t>(
      std::ceil(0.99 * static_cast<double>(sorted.size())));
  if (index == 0)
    index = 1;
  index = std::min(index, sorted.size()) - 1;
  double p99 = sorted[index];
  double budget = 1000.0 / s.hz;
  EXPECT_LE(p99, budget * 1.05);
}

TEST(RTPipeline, TraceIncludesCriticalEvents) {
  WorkerPool pool(3);
  SimCore::Settings s;
  s.hz = 60.0;
  s.maxFrames = 100;
  s.threads = 3;
  s.autoTuneChunks = false;
  s.chunkSize = 64;
  s.bintraceEnable = true;
  s.bintraceEventsPerThread = 1u << 14;
  s.budgetMonitor = false;
  SimCore sim(s);
  sim.setWorkerPool(&pool);

  sim.applyDegradeRung(1);

  auto rangePhase = sim.addPhase("range", 1024);
  sim.addParallelRangeTask(rangePhase,
                           [](std::size_t, std::size_t, std::int64_t,
                              SimCore::Seconds) { busy_for(0.2); });

  auto slowPhase = sim.addPhase("slow");
  sim.addSerialSubsystem(slowPhase, [](std::int64_t frame, SimCore::Seconds) {
    if (frame % 25 == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    else
      busy_for(1.0);
  });

  sim.run();

  EXPECT_GE(sim.watchdogTrips(), 1);

  auto frameSnapshot = sim.saveFrame();
  sim.loadFrame(frameSnapshot);

  simcore::hal::gpu::FrameGraph fg;
  fg.add_pass([]() { busy_for(1.0); },
              []() { std::this_thread::sleep_for(std::chrono::milliseconds(5)); });
  (void)fg.execute();

  auto snap = sim.bintrace().snapshot();

  std::ostringstream os;
  trace_export::write_chrome_trace(snap, os);
  auto json = os.str();
  EXPECT_FALSE(json.empty());

  auto countCat = [&json](const std::string &cat) {
    std::string pattern = "\"cat\":\"" + cat + "\"";
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = json.find(pattern, pos)) != std::string::npos) {
      ++count;
      pos += pattern.size();
    }
    return count;
  };

  EXPECT_GE(countCat("PhaseBegin"), 200u);
  EXPECT_GE(countCat("PhaseEnd"), 200u);
  EXPECT_GE(countCat("ChunkStart"), 1u);
  EXPECT_GE(countCat("ChunkDone"), 1u);
  EXPECT_GE(countCat("QueuePush"), 1u);
  EXPECT_GE(countCat("QueuePop"), 1u);
  EXPECT_GE(countCat("WorkSteal"), 1u);
  EXPECT_GE(countCat("GovernorRung"), 1u);
  EXPECT_GE(countCat("WatchdogTrip"), 1u);
  EXPECT_GE(countCat("GpuFenceWaitBegin"), 1u);
  EXPECT_GE(countCat("GpuFenceWaitEnd"), 1u);
  EXPECT_GE(countCat("SnapshotSave"), 1u);
  EXPECT_GE(countCat("SnapshotLoad"), 1u);

  pool.stop();
}


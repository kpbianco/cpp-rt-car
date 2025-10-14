#define RTFW_DISABLE_EMERGENCY_SPAWN 1

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>

#include <simcore/SimCore.hpp>
#include <simcore/bintrace.hpp>
#include <simcore/highres_clock.hpp>
#include <simcore/logger.hpp>
#include <simcore/metrics.hpp>
#include <simcore/worker_pool.hpp>
#include <hal/hal.hpp>

namespace {

using namespace std::chrono_literals;

void busy_for(double milliseconds) {
  const auto start = HighResClock::now();
  const auto target = static_cast<std::uint64_t>(milliseconds * 1'000'000.0);
  while (HighResClock::now() - start < target) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
}

} // namespace

TEST(RTSafety, WorstCaseStressRecovers) {
  namespace fs = std::filesystem;

  constexpr std::size_t kThreads = 3;
  constexpr std::size_t kQueueSize = 32;
  constexpr std::size_t kFrames = 96;

  const auto logPath = fs::temp_directory_path() / "rt_safety_worst_case.log";
  std::error_code ec;
  fs::remove(logPath, ec);

  auto sink = std::make_shared<Logger::AsyncRingFileSink>(
      logPath.string(), 4, std::chrono::milliseconds(25));

  Logger logger;
  logger.setLevel(Logger::Level::Trace);
  logger.addSink(sink);

  WorkerPool pool(kThreads, kQueueSize);

  std::atomic<double> thermalTemp{72.0};

  SimCore::Settings settings;
  settings.hz = 240.0;
  settings.maxFrames = static_cast<std::int64_t>(kFrames);
  settings.threads = kThreads;
  settings.chunkSize = 32;
  settings.autoTuneChunks = false;
  settings.bintraceEnable = true;
  settings.bintraceEventsPerThread = 1u << 14;
  settings.rateGovernorTargetUtil = 0.65;
  settings.rateGovernorHysteresis = 0.05;
  settings.predictiveEnable = false;
  settings.adaptive = false;
  settings.spinMicros = 50;
  settings.budgetMonitor = true;
  settings.thermalMonitor = true;
  settings.thermalLimpCelsius = 85.0;
  settings.readPackageTemp = [&thermalTemp]() {
    return thermalTemp.load(std::memory_order_relaxed);
  };

  SimCore sim(settings);
  sim.setWorkerPool(&pool);
  sim.setLogger(&logger);

  metrics::Registry registry;
  sim.setMetrics(&registry);

  const auto ingest = sim.addPhase("stress.ingest", 1024);
  const auto gpu = sim.addPhase("stress.gpu");
  const auto io = sim.addPhase("stress.io");
  const auto compose = sim.addPhase("stress.compose");

  sim.addParallelRangeTask(ingest,
                           [](std::size_t begin, std::size_t end, std::int64_t,
                              SimCore::Seconds) {
                             for (std::size_t i = begin; i < end; ++i) {
                               (void)i;
                               busy_for(0.015);
                             }
                           },
                           SimCore::TaskHint::Latency);

  sim.addSerialSubsystem(ingest, [&](std::int64_t frame, SimCore::Seconds) {
    busy_for(0.25);
    if (frame < 6) {
      for (std::size_t i = 0; i < kQueueSize * 2; ++i) {
        pool.submit([] { busy_for(0.35); }, WorkerPool::Priority::Normal,
                    WorkerPool::Category::CPU);
      }
    }
    for (int i = 0; i < 250; ++i) {
      logger.info("ingest frame={} burst={}", frame, i);
    }
    if (frame == 12) {
      thermalTemp.store(settings.thermalLimpCelsius + 8.0,
                        std::memory_order_relaxed);
    }
  });

  auto submitGpuFence = [&](double busyMs) {
    auto fence = simcore::hal::fence_create();
    auto fenceCopy = fence;
    pool.submit(
        [fenceCopy, busyMs]() mutable {
          busy_for(busyMs);
          simcore::hal::fence_signal(fenceCopy);
        },
        WorkerPool::Priority::Normal, WorkerPool::Category::GPU);
    return fence;
  };

  sim.addSerialSubsystem(gpu, [&](std::int64_t frame, SimCore::Seconds) {
    auto fenceSlow = submitGpuFence(1.4);
    auto fenceStall = submitGpuFence(1.0);
    sim.bintrace().log(bintrace::EV_GpuFenceWaitBegin, 2u,
                       static_cast<std::uint64_t>(frame));
    sim.fiberPool().wait_for_fence(std::move(fenceSlow));
    sim.fiberPool().wait_for_fence(std::move(fenceStall));
    sim.fiberPool().drain();
    sim.bintrace().log(bintrace::EV_GpuFenceWaitEnd, 2u,
                       static_cast<std::uint64_t>(frame));
    busy_for(0.3);
  });

  sim.addSerialSubsystem(io, [&](std::int64_t frame, SimCore::Seconds) {
    busy_for(0.4);
    if ((frame % 3) == 0) {
      pool.submit([] { busy_for(0.2); }, WorkerPool::Priority::Low,
                  WorkerPool::Category::IO);
    }
  });

  sim.addSerialSubsystem(compose, [&](std::int64_t frame, SimCore::Seconds) {
    busy_for(0.22);
    if ((frame % 8) == 0) {
      logger.warn("compose heartbeat frame={}", frame);
    }
  });

  ASSERT_TRUE(sim.addDependency(ingest, gpu));
  ASSERT_TRUE(sim.addDependency(ingest, io));
  ASSERT_TRUE(sim.addDependency(gpu, compose));
  ASSERT_TRUE(sim.addDependency(io, compose));

  sim.run();

  pool.drain();
  auto poolStats = pool.stats();
  pool.stop();

  auto snapshot = registry.snapshot();

  EXPECT_EQ(sim.frame(), static_cast<std::int64_t>(kFrames));
  EXPECT_GE(sim.watchdogTrips(), 1);
  EXPECT_TRUE(sim.limpModeActive());
  EXPECT_GE(sim.rungActivations(4), 1u);

  auto findCounter = [&](const char *name) {
    auto it = snapshot.counters.find(name);
    EXPECT_NE(it, snapshot.counters.end());
    return it != snapshot.counters.end() ? it->second : 0ull;
  };

  const auto missedFrames = findCounter("missed_frames");
  EXPECT_LE(missedFrames, 2u);
  EXPECT_GE(findCounter("watchdog.trips"), 1u);
  EXPECT_GE(findCounter("thermal.events"), 1u);
  const auto logDrops = findCounter("log_drops");
  EXPECT_GT(logDrops, 0u);
  const auto queueMax = findCounter("worker.queue_max");
  EXPECT_GE(queueMax, kQueueSize);
  EXPECT_EQ(findCounter("worker.steals_total"),
            static_cast<std::uint64_t>(poolStats.totalSteals));

  auto traceSnap = sim.bintrace().snapshot();
  std::size_t rungEvents = 0;
  std::size_t watchdogCrumbs = 0;
  std::size_t queuePushDepth = 0;
  std::size_t queuePopEvents = 0;
  std::size_t fenceBegin = 0;
  std::size_t fenceEnd = 0;

  for (const auto &ev : traceSnap.events) {
    switch (ev.code) {
    case bintrace::EV_BudgetLadder:
      ++rungEvents;
      break;
    case bintrace::EV_WatchdogTrip:
      ++watchdogCrumbs;
      break;
    case bintrace::EV_QueuePush:
      queuePushDepth = std::max<std::size_t>(queuePushDepth, ev.a);
      break;
    case bintrace::EV_QueuePop:
      ++queuePopEvents;
      break;
    case bintrace::EV_GpuFenceWaitBegin:
      ++fenceBegin;
      break;
    case bintrace::EV_GpuFenceWaitEnd:
      ++fenceEnd;
      break;
    default:
      break;
    }
  }

  EXPECT_GT(rungEvents, 0u);
  EXPECT_GT(watchdogCrumbs, 0u);
  EXPECT_GE(queuePushDepth, kQueueSize);
  EXPECT_GT(queuePopEvents, 0u);
  EXPECT_GE(fenceBegin, 1u);
  EXPECT_GE(fenceEnd, fenceBegin);

  EXPECT_GE(poolStats.maxQueueDepth, kQueueSize);
  EXPECT_EQ(poolStats.stealsPerThread.size(), kThreads);

  sink.reset();
  fs::remove(logPath, ec);
}

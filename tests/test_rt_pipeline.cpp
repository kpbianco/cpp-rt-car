#include <cstdint>
#include <numeric>

#include <gtest/gtest.h>

#include <rt/runtime.hpp>
#include <simcore/SimCore.hpp>
#include <simcore/bintrace.hpp>
#include <simcore/metrics.hpp>
#include <simcore/worker_pool.hpp>

namespace {

constexpr std::size_t kFrameCount = 300;

} // namespace

TEST(RTPipeline, EndToEndSmokeTest) {
  WorkerPool pool(2);

  SimCore::Settings settings;
  settings.hz = 400.0;
  settings.maxFrames = static_cast<std::int64_t>(kFrameCount);
  settings.threads = 3;
  settings.chunkSize = 64;
  settings.autoTuneChunks = false;
  settings.bintraceEnable = true;
  settings.bintraceEventsPerThread = 1u << 14;
  settings.rateGovernorTargetUtil = 0.6;
  settings.rateGovernorHysteresis = 0.05;

  SimCore sim(settings);
  sim.setWorkerPool(&pool);

  metrics::Registry registry;
  sim.setMetrics(&registry);

  auto pipeline = rt::build_demo_pipeline(sim);
  ASSERT_TRUE(pipeline.valid());

  const auto workerThreads = pool.thread_count();

  sim.run();

  pool.drain();
  auto stats = pool.stats();
  pool.stop();

  EXPECT_EQ(sim.frame(), static_cast<std::int64_t>(kFrameCount));
  EXPECT_EQ(pipeline.ingest_frames(), kFrameCount);
  EXPECT_EQ(pipeline.gpu_frames(), kFrameCount);
  EXPECT_EQ(pipeline.io_frames(), kFrameCount);
  EXPECT_EQ(pipeline.compose_frames(), kFrameCount);
  EXPECT_EQ(pipeline.fence_waits(), kFrameCount);

  EXPECT_GT(stats.totalSteals, 0u);
  ASSERT_EQ(stats.stealsPerThread.size(), workerThreads);
  const auto stealSum =
      std::accumulate(stats.stealsPerThread.begin(), stats.stealsPerThread.end(),
                      std::size_t{0});
  EXPECT_EQ(stealSum, stats.totalSteals);

  EXPECT_GE(sim.rungActivations(1), 1u);
  EXPECT_GE(sim.governorRungCount(1), 1u);
  EXPECT_LT(sim.governorScale(), 1.0);

  auto snapshot = registry.snapshot();
  auto expectCounter = [&](const char *name) -> std::uint64_t {
    auto it = snapshot.counters.find(name);
    EXPECT_NE(it, snapshot.counters.end());
    return it != snapshot.counters.end() ? it->second : 0ull;
  };

  const auto queueMax = expectCounter("worker.queue_max");
  EXPECT_GT(queueMax, 0ull);
  EXPECT_EQ(expectCounter("worker.steals_total"),
            static_cast<std::uint64_t>(stats.totalSteals));
  EXPECT_EQ(expectCounter("worker.emergency_spawns"),
            stats.emergencySpawns);
  EXPECT_NE(snapshot.counters.find("missed_frames"), snapshot.counters.end());
  EXPECT_NE(snapshot.counters.find("watchdog.trips"), snapshot.counters.end());
  EXPECT_NE(snapshot.counters.find("log_drops"), snapshot.counters.end());

  auto traceSnap = sim.bintrace().snapshot();
  std::size_t fenceBegin = 0;
  std::size_t fenceEnd = 0;
  std::size_t rungEvents = 0;
  std::size_t queuePush = 0;
  std::size_t queuePop = 0;
  std::size_t stealEvents = 0;
  for (const auto &ev : traceSnap.events) {
    switch (ev.code) {
    case bintrace::EV_GpuFenceWaitBegin:
      ++fenceBegin;
      break;
    case bintrace::EV_GpuFenceWaitEnd:
      ++fenceEnd;
      break;
    case bintrace::EV_BudgetLadder:
      ++rungEvents;
      break;
    case bintrace::EV_QueuePush:
      ++queuePush;
      break;
    case bintrace::EV_QueuePop:
      ++queuePop;
      break;
    case bintrace::EV_WorkSteal:
      ++stealEvents;
      break;
    default:
      break;
    }
  }

  EXPECT_GE(fenceBegin, 1u);
  EXPECT_GE(fenceEnd, fenceBegin);
  EXPECT_GE(rungEvents, 1u);
  EXPECT_GE(queuePush, 1u);
  EXPECT_GE(queuePop, 1u);
  EXPECT_GE(stealEvents, 1u);
}

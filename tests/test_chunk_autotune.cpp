#include <atomic>
#include <cstdio>
#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#ifndef _WIN32
#include <unistd.h>
#endif

static std::size_t
expectedTotalChunks(std::size_t elems, const SimCore::Settings &s,
                    SimCore::TaskHint hint = SimCore::TaskHint::Throughput) {
  if (!s.autoTuneChunks) {
    std::size_t fixed =
        std::max<std::size_t>(1, s.chunkSize ? s.chunkSize : 256);
    if (hint == SimCore::TaskHint::Latency)
      fixed = std::max<std::size_t>(1, fixed / 2);
    return (elems + fixed - 1) / fixed;
  }
  const std::size_t threads =
      std::max<std::size_t>(1, s.threads ? s.threads : 1);
  std::size_t targetChunks = std::max<std::size_t>(
      1,
      threads * static_cast<std::size_t>(std::max(1, s.targetChunksPerThread)));
  if (hint == SimCore::TaskHint::Latency)
    targetChunks *= 2;

  std::size_t chunk = (elems + targetChunks - 1) / targetChunks; // ceil
  if (chunk < s.minChunk)
    chunk = s.minChunk;
  if (chunk > s.maxChunk)
    chunk = s.maxChunk;
  if (chunk > elems)
    chunk = elems;
  if (chunk == 0)
    chunk = 1;
  return (elems + chunk - 1) / chunk; // total chunk count
}

TEST(ChunkAutoTune, ManyElementsTargetsChunksPerThread) {
  SimCore::Settings s;
  s.hz = 100.0;
  s.maxFrames = 1;
  s.threads = 4;
  s.mainHelps = true;
  s.autoTuneChunks = true;
  s.targetChunksPerThread = 2;
  s.minChunk = 256;
  s.maxChunk = 100000; // no upper clamp for this case

  const std::size_t elems = 10000;
  const std::size_t expected = expectedTotalChunks(elems, s);

  SimCore sim(s);
  std::atomic<int> calls{0};

  auto ph = sim.addPhase("P", elems);
  sim.addParallelRangeTask(
      ph, [&](std::size_t b, std::size_t e, int64_t, SimCore::Seconds) {
        (void)b;
        (void)e;
        calls.fetch_add(1, std::memory_order_relaxed);
      });

  sim.run();
  EXPECT_EQ(static_cast<std::size_t>(calls.load()), expected);
}

TEST(ChunkAutoTune, SmallArrayClampedByMinChunk) {
  SimCore::Settings s;
  s.hz = 100.0;
  s.maxFrames = 1;
  s.threads = 8;
  s.mainHelps = true;
  s.autoTuneChunks = true;
  s.targetChunksPerThread = 2;
  s.minChunk = 256;
  s.maxChunk = 4096;

  const std::size_t elems = 500; // would choose ~32, but minChunk clamps to 256
  const std::size_t expected = expectedTotalChunks(elems, s);

  SimCore sim(s);
  std::atomic<int> calls{0};

  auto ph = sim.addPhase("Small", elems);
  sim.addParallelRangeTask(
      ph, [&](std::size_t, std::size_t, int64_t, SimCore::Seconds) {
        calls.fetch_add(1, std::memory_order_relaxed);
      });

  sim.run();
  EXPECT_EQ(static_cast<std::size_t>(calls.load()),
            expected); // expect 2 chunks
}

TEST(ChunkAutoTune, DisabledUsesFixedChunk) {
  SimCore::Settings s;
  s.hz = 100.0;
  s.maxFrames = 1;
  s.threads = 3;
  s.mainHelps = true;
  s.autoTuneChunks = false; // disabled
  s.chunkSize = 128;        // fixed size

  const std::size_t elems = 1000;
  const std::size_t expected =
      expectedTotalChunks(elems, s); // ceil(1000/128)=8

  SimCore sim(s);
  std::atomic<int> calls{0};

  auto ph = sim.addPhase("Fixed", elems);
  sim.addParallelRangeTask(
      ph, [&](std::size_t, std::size_t, int64_t, SimCore::Seconds) {
        calls.fetch_add(1, std::memory_order_relaxed);
      });

  sim.run();
  EXPECT_EQ(static_cast<std::size_t>(calls.load()), expected);
}

TEST(ChunkAutoTune, HintLatencyProducesMoreChunks) {
  SimCore::Settings s;
  s.hz = 100.0;
  s.maxFrames = 1;
  s.threads = 4;
  s.mainHelps = true;
  s.autoTuneChunks = true;
  s.targetChunksPerThread = 2;
  s.minChunk = 64;
  s.maxChunk = 100000;

  const std::size_t elems = 10000;

  SimCore simT(s);
  std::atomic<int> callsT{0};
  auto phT = simT.addPhase("Thr", elems);
  simT.addParallelRangeTask(
      phT,
      [&](std::size_t, std::size_t, int64_t, SimCore::Seconds) {
        callsT.fetch_add(1, std::memory_order_relaxed);
      },
      SimCore::TaskHint::Throughput);
  simT.run();
  const std::size_t expectedT =
      expectedTotalChunks(elems, s, SimCore::TaskHint::Throughput);
  EXPECT_EQ(static_cast<std::size_t>(callsT.load()), expectedT);

  SimCore simL(s);
  std::atomic<int> callsL{0};
  auto phL = simL.addPhase("Lat", elems);
  simL.addParallelRangeTask(
      phL,
      [&](std::size_t, std::size_t, int64_t, SimCore::Seconds) {
        callsL.fetch_add(1, std::memory_order_relaxed);
      },
      SimCore::TaskHint::Latency);
  simL.run();
  const std::size_t expectedL =
      expectedTotalChunks(elems, s, SimCore::TaskHint::Latency);
  EXPECT_EQ(static_cast<std::size_t>(callsL.load()), expectedL);
  EXPECT_GT(expectedL, expectedT);
}

TEST(ChunkAutoTune, CachePinsPerMachine) {
  SimCore::Settings s;
  s.hz = 100.0;
  s.maxFrames = 3;
  s.threads = 4;
  s.mainHelps = true;
  s.autoTuneChunks = true;
  s.targetChunksPerThread = 2;
  s.minChunk = 64;
  s.maxChunk = 1024;
  s.chunkSampleFrames = 2; // pin quickly
  s.chunkPercentile = 1.0;
  s.chunkCacheFile = "chunk_cache_test";

#ifndef _WIN32
  char host[256];
  gethostname(host, sizeof(host));
  std::string path = s.chunkCacheFile + std::string(".") + host;
  std::remove(path.c_str());
#endif

  const std::size_t elems = 1000;
  SimCore sim1(s);
  auto ph1 = sim1.addPhase("Cache", elems);
  sim1.addParallelRangeTask(ph1, [](std::size_t, std::size_t, int64_t, SimCore::Seconds) {});
  sim1.run();
  std::size_t pinned = sim1.phaseChunk(ph1);
  EXPECT_TRUE(sim1.phaseChunkPinned(ph1));

  SimCore::Settings s2 = s;
  s2.targetChunksPerThread = 1; // would change chunk if not pinned
  SimCore sim2(s2);
  auto ph2 = sim2.addPhase("Cache", elems);
  sim2.addParallelRangeTask(ph2, [](std::size_t, std::size_t, int64_t, SimCore::Seconds) {});
  sim2.run();
  EXPECT_EQ(sim2.phaseChunk(ph2), pinned);

#ifndef _WIN32
  std::remove(path.c_str());
#endif
}

TEST(ChunkAutoTune, CusumDriftRetunes) {
  SimCore::Settings s;
  s.hz = 100.0;
  s.maxFrames = 3;
  s.threads = 4;
  s.mainHelps = true;
  s.autoTuneChunks = true;
  s.targetChunksPerThread = 2;
  s.minChunk = 64;
  s.maxChunk = 1024;
  s.chunkSampleFrames = 2;
  s.chunkPercentile = 1.0;
  s.chunkDriftThreshold = 100.0;
  s.chunkCacheFile = "chunk_cache_test2";

#ifndef _WIN32
  char host[256];
  gethostname(host, sizeof(host));
  std::string path = s.chunkCacheFile + std::string(".") + host;
  std::remove(path.c_str());
#endif

  const std::size_t elems = 1000;
  SimCore sim1(s);
  auto ph1 = sim1.addPhase("Drift", elems);
  sim1.addParallelRangeTask(ph1, [](std::size_t, std::size_t, int64_t, SimCore::Seconds) {});
  sim1.run();
  std::size_t pinned = sim1.phaseChunk(ph1);
  EXPECT_TRUE(sim1.phaseChunkPinned(ph1));

  SimCore::Settings s2 = s;
  s2.targetChunksPerThread = 10; // drastic change -> new calc small
  SimCore sim2(s2);
  auto ph2 = sim2.addPhase("Drift", elems);
  sim2.addParallelRangeTask(ph2, [](std::size_t, std::size_t, int64_t, SimCore::Seconds) {});
  sim2.run();
  std::size_t newChunk = sim2.phaseChunk(ph2);
  EXPECT_NE(newChunk, pinned);

#ifndef _WIN32
  std::remove(path.c_str());
#endif
}

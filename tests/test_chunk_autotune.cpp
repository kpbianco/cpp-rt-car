#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <thread>
#include <vector>

TEST(ChunkAutoTune, AdaptsToCostChanges) {
  SimCore::Settings s;
  s.hz = 100.0;
  s.maxFrames = 6;
  s.threads = 2;
  s.mainHelps = true;
  s.autoTuneChunks = true;
  s.minChunk = 1;
  s.maxChunk = 1000;
  s.targetChunkMicros = 200;

  const std::size_t elems = 1000;
  std::vector<int> costPerElem = {0, 0, 50, 50, 50, 50};
  std::vector<std::size_t> firstChunk;

  SimCore sim(s);
  auto ph = sim.addPhase("P", elems);
  sim.addParallelRangeTask(ph, [&](std::size_t b, std::size_t e,
                                   std::int64_t frame, SimCore::Seconds) {
    if (b == 0)
      firstChunk.push_back(e - b);
    int micros = costPerElem[static_cast<std::size_t>(frame)];
    if (micros > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(micros * (e - b)));
    }
  });

  sim.run();

  ASSERT_EQ(firstChunk.size(), costPerElem.size());
  // After initial measurement of cheap cost, chunk should expand to full range
  EXPECT_EQ(firstChunk[1], elems);
  // After several expensive frames, chunk size should shrink substantially
  EXPECT_LT(firstChunk.back(), firstChunk[1] / 4);
}

TEST(ChunkAutoTune, DisabledUsesFixedChunk) {
  SimCore::Settings s;
  s.hz = 100.0;
  s.maxFrames = 1;
  s.threads = 3;
  s.mainHelps = true;
  s.autoTuneChunks = false;
  s.chunkSize = 128;

  const std::size_t elems = 1000;
  const std::size_t expected = (elems + s.chunkSize - 1) / s.chunkSize;

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

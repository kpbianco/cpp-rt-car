#include "gtest/gtest.h"
#include "SimCore.hpp"
#include <atomic>

TEST(SmallArrayBalance, SerialOnTinyArrays) {
    SimCore::Settings s;
    s.hz = 200.0;
    s.maxFrames = 1;
    s.threads = 8;
    s.autoTuneChunks = true;
    s.targetChunksPerThread = 2;
    s.minChunk = 64;
    s.maxChunk = 8192;
    s.minParallelElems = 0;       // auto = 2*minChunk = 128
    s.minParallelChunks = 2;

    SimCore sim(s);

    // Tiny array: 100 < 128 -> serial expected
    auto p = sim.addPhase("tiny", 100);
    std::atomic<int> calls{0};
    sim.addParallelRangeTask(p, [&](std::size_t b, std::size_t e, std::int64_t, SimCore::Seconds){
        (void)b; (void)e; calls.fetch_add(1, std::memory_order_relaxed);
    });

    sim.run();
    EXPECT_EQ(calls.load(), 1) << "tiny arrays should execute as one serial chunk";
}

TEST(SmallArrayBalance, ParallelWhenWorthIt) {
    SimCore::Settings s;
    s.hz = 200.0;
    s.maxFrames = 1;
    s.threads = 8;
    s.autoTuneChunks = true;
    s.targetChunksPerThread = 2;
    s.minChunk = 64;
    s.maxChunk = 8192;
    s.minParallelElems = 0;       // auto = 128
    s.minParallelChunks = 2;

    SimCore sim(s);

    // Large enough: 32768 elems -> should split to many chunks
    auto p = sim.addPhase("big", 32768);
    std::atomic<int> calls{0};
    sim.addParallelRangeTask(p, [&](std::size_t b, std::size_t e, std::int64_t, SimCore::Seconds){
        (void)b; (void)e; calls.fetch_add(1, std::memory_order_relaxed);
    });

    sim.run();
    EXPECT_GT(calls.load(), 1) << "large arrays should be split across chunks";
}

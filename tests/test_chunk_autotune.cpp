#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <atomic>

static std::size_t expectedTotalChunks(std::size_t elems,
                                       const SimCore::Settings& s)
{
    if (!s.autoTuneChunks) {
        const std::size_t fixed = std::max<std::size_t>(1, s.chunkSize ? s.chunkSize : 256);
        return (elems + fixed - 1) / fixed;
    }
    const std::size_t threads = std::max<std::size_t>(1, s.threads ? s.threads : 1);
    const std::size_t targetChunks =
        std::max<std::size_t>(1, threads * static_cast<std::size_t>(std::max(1, s.targetChunksPerThread)));

    std::size_t chunk = (elems + targetChunks - 1) / targetChunks; // ceil
    if (chunk < s.minChunk) chunk = s.minChunk;
    if (chunk > s.maxChunk) chunk = s.maxChunk;
    if (chunk > elems)      chunk = elems;
    if (chunk == 0)         chunk = 1;
    return (elems + chunk - 1) / chunk; // total chunk count
}

TEST(ChunkAutoTune, ManyElementsTargetsChunksPerThread)
{
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
    sim.addParallelRangeTask(ph, [&](std::size_t b, std::size_t e, int64_t, SimCore::Seconds){
        (void)b; (void)e;
        calls.fetch_add(1, std::memory_order_relaxed);
    });

    sim.run();
    EXPECT_EQ(static_cast<std::size_t>(calls.load()), expected);
}

TEST(ChunkAutoTune, SmallArrayClampedByMinChunk)
{
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
    sim.addParallelRangeTask(ph, [&](std::size_t, std::size_t, int64_t, SimCore::Seconds){
        calls.fetch_add(1, std::memory_order_relaxed);
    });

    sim.run();
    EXPECT_EQ(static_cast<std::size_t>(calls.load()), expected); // expect 2 chunks
}

TEST(ChunkAutoTune, DisabledUsesFixedChunk)
{
    SimCore::Settings s;
    s.hz = 100.0;
    s.maxFrames = 1;
    s.threads = 3;
    s.mainHelps = true;
    s.autoTuneChunks = false;     // disabled
    s.chunkSize = 128;            // fixed size

    const std::size_t elems = 1000;
    const std::size_t expected = expectedTotalChunks(elems, s); // ceil(1000/128)=8

    SimCore sim(s);
    std::atomic<int> calls{0};

    auto ph = sim.addPhase("Fixed", elems);
    sim.addParallelRangeTask(ph, [&](std::size_t, std::size_t, int64_t, SimCore::Seconds){
        calls.fetch_add(1, std::memory_order_relaxed);
    });

    sim.run();
    EXPECT_EQ(static_cast<std::size_t>(calls.load()), expected);
}

#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <cstdint>

TEST(BinaryTrace, RecordsPhasesAndChunks) {
    SimCore::Settings s;
    s.hz = 200.0;
    s.maxFrames = 20;
    s.threads = 4;
    s.autoTuneChunks = true;
    s.targetChunksPerThread = 2;
    s.detReduceLeaf = 256;
    s.bintraceEnable = true;
    s.bintraceEventsPerThread = 1u << 14; // 16k events per thread (small for test)
    s.budgetMonitor = false;

    SimCore sim(s);

    // Simple phase with 4096 elements -> chunked across workers
    const std::size_t phase = sim.addPhase("TraceMe", 4096);

    // trivial range task
    sim.addParallelRangeTask(phase, [](std::size_t b, std::size_t e, std::int64_t, SimCore::Seconds){
        // do a tiny deterministic op
        volatile double x = 0.0;
        for (std::size_t i=b; i<e; ++i) x += i * 0.000001;
        (void)x;
    });

    // one serial reduction just to have more markers
    sim.addReductionTask(phase, [](std::int64_t, SimCore::Seconds){});

    // run a few frames
    sim.run();

    // Snapshot AFTER steps
    auto snap = sim.bintrace().snapshot();

    // We expect at least some events across threads and codes present.
    std::size_t total = snap.events.size();
    ASSERT_GT(total, 0u);

    bool sawPhaseBegin = false, sawPhaseEnd = false, sawChunkStart = false, sawChunkDone = false;
    for (const auto& ev : snap.events) {
        if (ev.code == bintrace::EV_PhaseBegin) sawPhaseBegin = true;
        if (ev.code == bintrace::EV_PhaseEnd)   sawPhaseEnd   = true;
        if (ev.code == bintrace::EV_ChunkStart) sawChunkStart = true;
        if (ev.code == bintrace::EV_ChunkDone)  sawChunkDone  = true;
    }
    EXPECT_TRUE(sawPhaseBegin);
    EXPECT_TRUE(sawPhaseEnd);
    EXPECT_TRUE(sawChunkStart);
    EXPECT_TRUE(sawChunkDone);

    // Optional: write file (not asserted). Should succeed if FS is available.
    (void)sim.bintrace().writeFile("trace.btrc");
}

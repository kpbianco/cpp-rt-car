#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/logger.hpp>

TEST(PhaseTelemetry, EmitsMetrics) {
    SimCore::Settings s;
    s.hz = 60.0;
    s.maxFrames = 1;
    s.threads = 3;
    s.autoTuneChunks = false;
    s.chunkSize = 64;
    s.driftLogInterval = 0;

    Logger log; log.setLevel(Logger::Level::Trace);
    auto sink = std::make_shared<Logger::RingBufferSink>();
    log.addSink(sink);

    SimCore sim(s);
    sim.setLogger(&log);

    auto p = sim.addPhase("tele", 150);
    sim.addParallelRangeTask(p, [](std::size_t, std::size_t, std::int64_t, SimCore::Seconds){});

    sim.run();

    auto lines = sink->snapshot();
    bool found = false;
    for (const auto& ln : lines) {
        if (ln.find("phase tele: chunk=64 total=3 skew=3") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}


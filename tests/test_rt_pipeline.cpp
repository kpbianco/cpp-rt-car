#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/worker_pool.hpp>
#include <chrono>
#include <thread>
#include <fstream>
#include <cstdlib>

// Ensure the worker pool backs all phase work and emits metrics.
TEST(RtPipeline, PoolMetricsPopulated) {
    SimCore::Settings s;
    s.hz = 60.0;
    s.maxFrames = 1;
    s.threads = 2;
    s.autoTuneChunks = false;
    s.chunkSize = 1;
    s.driftLogInterval = 0;

    WorkerPool pool(2);
    SimCore sim(s);
    sim.setWorkerPool(&pool);

    for (int i = 0; i < 8; ++i) {
        auto p = sim.addPhase("p" + std::to_string(i));
        sim.addSerialSubsystem(p, [](std::int64_t, SimCore::Seconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
    }

    sim.run();

    EXPECT_GT(pool.max_queue_depth(), 0u);
    EXPECT_GT(pool.steals(), 0u);
}

// Lint to ensure phases are not spawning raw threads.
TEST(RtPipeline, NoRawThreadSpawns) {
    int rc = std::system("rg -l 'std::thread' src | wc -l > thread_count.txt");
    ASSERT_EQ(rc, 0);
    std::ifstream in("thread_count.txt");
    int count = 0;
    in >> count;
    std::remove("thread_count.txt");
    EXPECT_EQ(count, 0);
}


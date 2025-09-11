#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

// NOTE: include via a path relative to /tests
#include <simcore/SimCore.hpp>
#include <simcore/logger.hpp>
#include <simcore/worker_pool.hpp>

using Clock = std::chrono::steady_clock;

TEST(PhaseGraph, EnforcesDepsAndRunsParallelFrontier)
{
    Logger log;
    log.setLevel(Logger::Level::Warn);

    SimCore::Settings s;
    s.hz = 1000.0;
    s.maxFrames = 1;
    s.threads = std::max(2u, std::thread::hardware_concurrency());
    s.logPhases = false;

    SimCore sim(s);
    sim.setLogger(&log);

    WorkerPool pool(/*threads*/3, /*queue size pow2*/1024);
    sim.setWorkerPool(&pool);

    auto A = sim.addPhase("A");
    auto B = sim.addPhase("B");
    auto C = sim.addPhase("C");
    auto D = sim.addPhase("D");

    ASSERT_TRUE(sim.addDependency(A, C));
    ASSERT_TRUE(sim.addDependency(B, C));
    ASSERT_TRUE(sim.addDependency(C, D));

    Clock::time_point startA, endA, startB, endB, startC, endC, startD, endD;

    sim.addSerialSubsystem(A, [&](std::int64_t, SimCore::Seconds){
        startA = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        endA = Clock::now();
    });
    sim.addSerialSubsystem(B, [&](std::int64_t, SimCore::Seconds){
        startB = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        endB = Clock::now();
    });
    sim.addSerialSubsystem(C, [&](std::int64_t, SimCore::Seconds){
        startC = Clock::now();
        endC = Clock::now();
    });
    sim.addSerialSubsystem(D, [&](std::int64_t, SimCore::Seconds){
        startD = Clock::now();
        endD = Clock::now();
    });

    sim.run();

    // C must start after A and B finished
    EXPECT_GE(startC, endA);
    EXPECT_GE(startC, endB);

    // D must start after C finished
    EXPECT_GE(startD, endC);

    // A and B should overlap -> positive overlap duration
    auto overlap = std::min(endA,endB) - std::max(startA,startB);
    EXPECT_GT(std::chrono::duration_cast<std::chrono::milliseconds>(overlap).count(), 0);
}

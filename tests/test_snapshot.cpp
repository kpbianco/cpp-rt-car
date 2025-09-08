#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/logger.hpp>
#include <rt/snapshot.hpp>
#include <unordered_map>
#include <vector>

TEST(SnapshotRollback, ReplayHashEquality) {
    const int N = 40;
    const int TOTAL = 60; // N + 20 frames

    SimCore::Settings s;
    s.hz = 60.0;
    s.maxFrames = N;
    s.threads = 1;
    s.adaptive = false;
    s.predictiveEnable = false;
    s.budgetMonitor = false;
    s.autoTuneChunks = false;
    s.driftLogInterval = 0;

    Logger log; log.setLevel(Logger::Level::Error);
    SimCore sim(s);
    sim.setLogger(&log);

    std::vector<int> dense(32, 0);
    std::unordered_map<int,int> sparse;
    for (int i = 0; i < 16; ++i) sparse[i * 2] = i;

    const std::size_t phase = sim.addPhase("P");
    sim.addSerialSubsystem(phase, [&](std::int64_t f, SimCore::Seconds){
        for (std::size_t i = 0; i < dense.size(); ++i)
            dense[i] += int(sim.prng(f, i) & 0xff);
        for (auto &kv : sparse)
            kv.second += int(sim.prng(f, kv.first) & 0xff);
    });

    sim.run(); // run N frames

    rt::SnapshotWriter ecsSnap;
    ecsSnap.writeVector(dense);
    ecsSnap.writeMap(sparse);
    std::vector<std::uint8_t> simSnap = sim.saveFrame();

    auto s2 = s;
    s2.maxFrames = TOTAL;
    sim.applySettings(s2);
    sim.run(); // run remaining frames

    rt::SnapshotWriter final1;
    final1.writeVector(dense);
    final1.writeMap(sparse);
    auto hash1 = rt::hash64(final1.data);

    dense.clear();
    sparse.clear();
    rt::SnapshotReader ecsRead(ecsSnap.data);
    ecsRead.readVector(dense);
    ecsRead.readMap(sparse);
    sim.loadFrame(simSnap);
    sim.run();

    rt::SnapshotWriter final2;
    final2.writeVector(dense);
    final2.writeMap(sparse);
    auto hash2 = rt::hash64(final2.data);

    EXPECT_EQ(hash1, hash2);
}

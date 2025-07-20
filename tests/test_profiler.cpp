#include <gtest/gtest.h>
#include "simcore.hpp"
#include "logger.hpp"
#include "profiler.hpp"

TEST(ProfilerIntegration, CollectsPhaseAndFrame) {
#ifdef PROF_ENABLED
    SimCore::Settings s;
    s.hz = 200.0;
    s.maxFrames = 100;
    s.threads = 1;
    s.driftLogInterval = 0;

    Logger log; log.setLevel(Logger::Level::Error);
    Profiler prof;

    SimCore sim(s);
    sim.setLogger(&log);
    sim.setProfiler(&prof);

    auto phase = sim.addPhase("Work");
    sim.addSerialSubsystem(phase, [&](int64_t, SimCore::Seconds){ volatile int x=0; for(int i=0;i<1000;++i) x+=i; });

    sim.run();

    auto rows = prof.summary();
    bool foundFrame=false, foundPhase=false;
    for (auto& e : rows) {
        if (e.name.rfind("Frame",0)==0) foundFrame=true;
        if (e.name.rfind("Phase:Work",0)==0) foundPhase=true;
    }
    EXPECT_TRUE(foundFrame);
    EXPECT_TRUE(foundPhase);
#else
    GTEST_SKIP() << "Profiler disabled";
#endif
}

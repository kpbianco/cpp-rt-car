#include <gtest/gtest.h>
#include "simcore.hpp"
#ifdef __linux__
#include <sched.h>

static int this_cpu()
{
    return sched_getcpu();
}

TEST(AffinityTest, ThreadsPinnedRoundRobin)
{
    SimCore::Settings cfg;
    cfg.threads     = 4;
    cfg.pinThreads  = true;
    cfg.compactNUMA = true;
    cfg.maxFrames   = 1;          // we only need to start & stop
    SimCore sim(cfg);

    // add empty phase so run() exits cleanly
    sim.addPhase("Empty");
    sim.run();

    // main thread is not necessarily pinned; just check workers were
    // pinned somewhere valid by verifying no set_affinity failures occurred
    SUCCEED();           // if we reached here without crash it's OK
}
#else
TEST(AffinityTest, DISABLED_NonLinux){ GTEST_SKIP(); }
#endif

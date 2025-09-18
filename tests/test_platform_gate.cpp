#include <gtest/gtest.h>

#include "simcore/bintrace.hpp"
#include "simcore/logger.hpp"
#include "simcore/platform_init.hpp"

TEST(PlatformGate, EmitsTraceCrumb) {
    bintrace::Trace trace;
    trace.init(1, 16, true);
    trace.bindThread(0);

    Logger logger;
    logger.setLevel(Logger::Level::Trace);

    simcore::PlatformInitOptions opts;
    opts.enable = true;

    auto state = simcore::platform_init_rt(opts, &logger, &trace);
    EXPECT_TRUE(state.requested);

    if (state.fifoPriority > 0) {
        simcore::platform_apply_worker_policy(state, &logger, &trace);
    }

    auto snapshot = trace.snapshot();
    bool foundCrumb = false;
    for (const auto &ev : snapshot.events) {
        if (ev.code == bintrace::EV_PlatformCrumb) {
            foundCrumb = true;
            break;
        }
    }
    EXPECT_TRUE(foundCrumb);

    trace.shutdown();
}

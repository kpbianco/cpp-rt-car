#include <gtest/gtest.h>
#include <simcore/frame_arena.hpp>

// This test relies on assertions being enabled (no -DNDEBUG)
TEST(RtArena, DeathOnOverflow) {
    FrameArena a(256, 64); // small capacity to trigger overflow easily
    (void)a.allocate(128, 64);
    (void)a.allocate(128, 64);
    EXPECT_DEATH({ (void)a.allocate(64, 64); }, "overflow");
}

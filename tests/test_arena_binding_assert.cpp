#include <gtest/gtest.h>
#include <simcore/frame_arena.hpp>

TEST(FrameArenaBindingAssert, DeathBeforeBinding) {
#ifdef NDEBUG
  GTEST_SKIP() << "Debug-only arena binding assert disabled in release builds";
#else
  FrameArenaPool pool(1, 1u << 12, 64);
  tls_arena_bound = false;
  EXPECT_FALSE(isCurrentThreadArenaBound());

  EXPECT_DEATH(
      {
        tls_arena_bound = false;
        RTFW_DEBUG_ASSERT(tls_arena_bound &&
                          "Thread arena not bound—call bindCurrentThread() before allocating");
        auto &arena = pool.tls();
        (void)arena.allocate(64, 64);
      },
      "Thread arena not bound");
#endif
}

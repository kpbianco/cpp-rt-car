#include <cstdint>
#include <gtest/gtest.h>
#include <simcore/fault_injector.hpp>
#include <simcore/frame_arena.hpp>

TEST(RtArena, OverflowTriggersDegrade) {
    FrameArena a(256, 64); // small capacity to trigger overflow easily
    auto* p0 = static_cast<std::uint8_t*>(a.allocate(128, 64));
    ASSERT_NE(p0, nullptr);

#ifdef NDEBUG
    void* fallback = nullptr;
    auto status = a.allocate(200, &fallback, 64);
    EXPECT_EQ(status, FrameArena::AllocationStatus::Degraded);
    EXPECT_TRUE(a.degraded());
    ASSERT_NE(fallback, nullptr);
    EXPECT_EQ(a.lastStatus(), FrameArena::AllocationStatus::Degraded);
    EXPECT_GT(a.fallbackBytes(), 0u);

    auto* p1 = static_cast<std::uint8_t*>(a.allocate(16, 64));
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(a.lastStatus(), FrameArena::AllocationStatus::Degraded);
#else
    EXPECT_THROW(a.allocate(200, 64), std::bad_alloc);
#endif
}

TEST(RtArena, FaultInjectedFallbackFailsGracefully) {
    FrameArena a(128, 64);
    FrameArena::setFallbackProvider(&fault::maybe_fail_alloc);
    fault::set_alloc_failure_probability(1.0);

#ifdef NDEBUG
    void* fallback = nullptr;
    auto status = a.allocate(256, &fallback, 64);
    EXPECT_EQ(status, FrameArena::AllocationStatus::Failed);
    EXPECT_TRUE(a.degraded());
    EXPECT_EQ(fallback, nullptr);
    EXPECT_EQ(a.lastStatus(), FrameArena::AllocationStatus::Failed);

    a.reset();
    EXPECT_TRUE(a.degraded());
    a.clearDegraded();
    EXPECT_FALSE(a.degraded());
#else
    EXPECT_THROW(a.allocate(256, 64), std::bad_alloc);
#endif

    FrameArena::resetFallbackProvider();
    fault::set_alloc_failure_probability(0.0);
}

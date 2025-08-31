#include <gtest/gtest.h>
#include "frame_arena.hpp"  // shim -> rt_memory.hpp

TEST(RtArena, BasicAllocAndReset) {
    FrameArena a(1024, 64); // 1KB arena

    auto* p0 = static_cast<std::uint8_t*>(a.allocate(16, 64));
    ASSERT_NE(p0, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p0) % 64, 0u);
    const auto used_after_16 = a.used();

    auto* p1 = static_cast<std::uint8_t*>(a.allocate(32, 32));
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p1) % 32, 0u);
    EXPECT_GT(a.used(), used_after_16);

    a.reset();
    EXPECT_EQ(a.used(), 0u);

    auto* p2 = static_cast<std::uint8_t*>(a.allocate(64, 64));
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p2) % 64, 0u);
}

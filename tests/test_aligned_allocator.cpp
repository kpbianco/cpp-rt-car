#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <simcore/aligned_allocator.hpp>

TEST(AlignedAllocator, Allocates64ByteAligned) {
    using Alloc64 = AlignedAllocator<float, 64>;
    std::vector<float, Alloc64> vec;
    vec.resize(8);
    auto* ptr = vec.data();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(ptr) % 64, 0u);
}


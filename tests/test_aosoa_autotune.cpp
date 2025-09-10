#include <gtest/gtest.h>
#include <simcore/soa/autotune.hpp>

TEST(AoSoA, AutotuneReturnsValidCandidate) {
    std::size_t n = 4096; // large enough to trigger prefetch heuristics
    auto tile = soa::autotune_block_size(n);
    EXPECT_TRUE(tile == 64 || tile == 128 || tile == 256 || tile == 512);
}

#include <gtest/gtest.h>
#include <simcore/car_soa.hpp>
#include <cstdint>

// Verify basic block characteristics
TEST(CarSoA, BlockLayout)
{
    EXPECT_EQ(CarSoA::block_size, 128u);

    CarSoA::Block blk{};
    EXPECT_EQ(sizeof(blk.pos) / sizeof(double), CarSoA::block_size);
    EXPECT_EQ(sizeof(blk.vel) / sizeof(double), CarSoA::block_size);

    EXPECT_EQ(alignof(CarSoA::Block), 64u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(blk.pos) % 64, 0u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(blk.vel) % 64, 0u);
}

// Ensure iteration across blocks including padded tail
TEST(CarSoA, IterationWithPadding)
{
    constexpr std::size_t N = CarSoA::block_size * 2 + 5;
    CarSoA cars(N);

    for (std::size_t i = 0; i < N; ++i)
        cars.set(i, static_cast<double>(i), static_cast<double>(i * 2));

    for (std::size_t i = 0; i < N; ++i) {
        EXPECT_EQ(cars.position(i), static_cast<double>(i));
        EXPECT_EQ(cars.velocity(i), static_cast<double>(i * 2));
    }

    std::size_t idx = 0;
    for (const auto& blk : cars.blocks()) {
        for (std::size_t j = 0; j < CarSoA::block_size; ++j) {
            if (idx < N) {
                EXPECT_EQ(blk.pos[j], static_cast<double>(idx));
                EXPECT_EQ(blk.vel[j], static_cast<double>(idx * 2));
            }
            ++idx;
        }
    }
    EXPECT_EQ(idx, cars.blocks().size() * CarSoA::block_size);
}


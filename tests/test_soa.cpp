#include <gtest/gtest.h>
#include <simcore/car_soa.hpp>

// basic insertion/removal and mapping checks
TEST(CarSoA, InsertRemove)
{
    CarSoA cars;
    cars.insert(2, 1.0, 2.0);
    cars.insert(5, 3.0, 4.0);

    EXPECT_EQ(cars.size(), 2u);
    EXPECT_EQ(*cars.lookup_position(2), 1.0);
    EXPECT_EQ(*cars.lookup_velocity(5), 4.0);

    // missing component: should point to default without branching
    EXPECT_EQ(cars.lookup_position(3), &cars.defaultPos);
    EXPECT_EQ(*cars.lookup_velocity(3), 0.0);

    cars.remove(2);
    EXPECT_EQ(cars.size(), 1u);
    EXPECT_EQ(cars.lookup_position(2), &cars.defaultPos);
    ASSERT_EQ(cars.dense[0], 5u); // remaining entity id
}

// verify dense arrays stay contiguous
TEST(CarSoA, ContiguousStorage)
{
    constexpr std::size_t N = 128;
    CarSoA cars;
    for (std::size_t i = 0; i < N; ++i)
        cars.insert(i, double(i), double(i * 2));

    for (std::size_t i = 0; i < N; ++i) {
        EXPECT_EQ(*cars.lookup_position(i), double(i));
        EXPECT_EQ(*cars.lookup_velocity(i), double(i * 2));
    }

    for (std::size_t i = 1; i < N; ++i)
        ASSERT_EQ(reinterpret_cast<const char*>(&cars.pos[i]) -
                  reinterpret_cast<const char*>(&cars.pos[i - 1]),
                  sizeof(double));
}

// ensure lookup can be used without branching for missing ids
TEST(CarSoA, BranchlessLookup)
{
    CarSoA cars;
    cars.insert(7, 3.0, 4.0); // ensure sparse size >= 8
    double v = *cars.lookup_velocity(3); // entity 3 missing
    EXPECT_EQ(v, 0.0);
    EXPECT_EQ(cars.lookup_velocity(3), &cars.defaultVel);
}


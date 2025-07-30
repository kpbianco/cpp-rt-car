#include <gtest/gtest.h>
#include "car_soa.hpp"

TEST(SoAData, ContiguousAndCorrect)
{
    constexpr std::size_t N = 1024;
    CarSoA cars(N);

    /* write pattern */
    for (std::size_t i = 0; i < N; ++i)
        cars.set(i, double(i), double(i * 2));

    /* verify */
    for (std::size_t i = 0; i < N; ++i) {
        EXPECT_EQ(cars.position(i), double(i));
        EXPECT_EQ(cars.velocity(i), double(i * 2));
    }

    /* contiguous check: difference of adjacent addresses = sizeof(double) */
    const double* base = cars.pos.data();
    for (std::size_t i = 1; i < N; ++i)
        ASSERT_EQ(reinterpret_cast<const char*>(&cars.pos[i]) -
                  reinterpret_cast<const char*>(&cars.pos[i - 1]),
                  sizeof(double));
}

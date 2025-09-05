#include <gtest/gtest.h>
#include <simcore/soa_view.hpp>

struct LegacyVec3 { double x, y, z; };

TEST(SOAView, Access)
{
    LegacyVec3 arr[4] = {{1,2,3},{4,5,6},{7,8,9},{10,11,12}};
    auto view = soa::Vec3AoSView<double>::from(arr, &LegacyVec3::x, &LegacyVec3::y, &LegacyVec3::z);
    view.x[1] = 40.0;
    EXPECT_EQ(arr[1].x, 40.0);
    EXPECT_EQ(view.y[2], 8.0);
}

TEST(SOAView, Axpy)
{
    LegacyVec3 pos[3] = {{0,0,0},{0,0,0},{0,0,0}};
    LegacyVec3 vel[3] = {{1,2,3},{4,5,6},{7,8,9}};
    auto pview = soa::Vec3AoSView<double>::from(pos, &LegacyVec3::x, &LegacyVec3::y, &LegacyVec3::z);
    auto vview = soa::Vec3AoSView<double>::from(vel, &LegacyVec3::x, &LegacyVec3::y, &LegacyVec3::z);
    soa::axpy3(vview, pview, 2.0, 3);
    EXPECT_DOUBLE_EQ(pos[2].z, 9.0 * 2.0);
    EXPECT_DOUBLE_EQ(pos[1].y, 5.0 * 2.0);
}


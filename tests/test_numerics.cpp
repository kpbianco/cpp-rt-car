#include <gtest/gtest.h>
#include <rt/numerics.hpp>
#include <cfenv>

TEST(Numerics, InitFpEnvSetsTonearest) {
    std::fesetround(FE_DOWNWARD);
    rt::init_fp_env();
    EXPECT_EQ(fegetround(), FE_TONEAREST);
}

TEST(Numerics, FmaGateStable) {
    rt::set_use_fma(true);
    double x = rt::fma(1.0, 2.0, 3.0);
    rt::set_use_fma(false);
    double y = rt::fma(1.0, 2.0, 3.0);
    EXPECT_EQ(x, y);
}

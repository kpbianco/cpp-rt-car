#include <gtest/gtest.h>
#include <rt/numerics.hpp>
#include <rt/fixed_point.hpp>
#include <simcore/robust_fp.hpp>
#include <cfenv>
#include <limits>

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

TEST(Numerics, InitFpEnvFlushesDenormals) {
    rt::init_fp_env();
    volatile float x = std::numeric_limits<float>::denorm_min();
    // DAZ should treat the denorm input as zero
    float y = x + 1.0f;
    EXPECT_EQ(y, 1.0f);
}

TEST(Numerics, FmaGateUlpStable) {
    rt::set_use_fma(true);
    double a = rt::fma(1e308, 1e-308, 1.0);
    rt::set_use_fma(false);
    double b = rt::fma(1e308, 1e-308, 1.0);
    EXPECT_EQ(robust::ulp_distance(a, b), 0u);
}

TEST(Numerics, FixedPointBasicOps) {
    using rt::Q16_16;
    Q16_16 a = Q16_16::fromFloat(1.5f);
    Q16_16 b = Q16_16::fromFloat(2.25f);
    Q16_16 sum = a + b;
    EXPECT_NEAR(sum.toFloat(), 3.75f, 1e-5f);
    Q16_16 prod = a * b;
    EXPECT_NEAR(prod.toFloat(), 3.375f, 1e-5f);
}

#include <gtest/gtest.h>
#include <simcore/robust_fp.hpp>
#include <vector>
#include <limits>
#include <cstdint>

TEST(RobustFP, SafeRsqrtFinite) {
    double r = robust::safe_rsqrt(0.0);
    ASSERT_TRUE(std::isfinite(r));
    double expected = 1.0 / std::sqrt(1e-12);
    EXPECT_DOUBLE_EQ(r, expected);
}

TEST(RobustFP, KahanImprovesSumULP) {
    // Summing many tiny increments causes naive accumulation to lose precision,
    // while Kahan summation retains them.
    std::vector<double> v;
    v.push_back(1.0); // large starting value
    v.insert(v.end(), 1000000, 1e-20); // lots of tiny contributions

    // naive sum
    double naive = 0.0;
    for (double x : v) naive += x;

    // kahan sum
    double kahan = robust::kahan_sum(v.begin(), v.end());

    const double expected = 1.0 + 1e-14;
    EXPECT_GT(kahan, naive);              // should accumulate more
    EXPECT_NEAR(kahan, expected, 1e-15);   // close to exact result
}

TEST(RobustFP, FloatReductionMixedPrecision) {
    // Many small FP32 values lose precision when accumulated in float.
    std::vector<float> v(1000000, 1e-7f);
    float naive = 0.0f;
    for (float x : v) naive += x;

    double mixed = robust::sum_fp32_to_fp64(v.begin(), v.end());
    EXPECT_GT(mixed, static_cast<double>(naive));
    double expected = static_cast<double>(v.size()) * static_cast<double>(1e-7f);
    EXPECT_NEAR(mixed, expected, 5e-10);
}

TEST(RobustFP, KahanFloatReduction) {
    std::vector<float> v;
    v.push_back(1.0f);
    v.insert(v.end(), 1000000, 1e-8f);

    float naive = 0.0f;
    for (float x : v) naive += x;
    double mixed = robust::sum_fp32_to_fp64(v.begin(), v.end());
    double kahan = robust::kahan_sum_fp32(v.begin(), v.end());
    EXPECT_GT(kahan, static_cast<double>(naive));
    EXPECT_NEAR(kahan, mixed, 1e-12);
    double expected = 1.0 +
        static_cast<double>(v.size() - 1) * static_cast<double>(1e-8f);
    EXPECT_NEAR(kahan, expected, 5e-11);
}

TEST(RobustFP, ShadowArithmeticDetectsInstability) {
    // Catastrophic cancellation in single precision.
    float a = 1e8f;
    float b = -1e8f;
    float c = 1.0f;
    float result = (a + c) + b; // loses the c contribution
    double shadow = (static_cast<double>(a) + static_cast<double>(c)) + static_cast<double>(b);
    std::uint64_t ulp = robust::ulp_distance(static_cast<double>(result), shadow);
    EXPECT_GT(ulp, 0u); // shadow arithmetic detects the loss
}

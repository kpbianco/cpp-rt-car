#include <gtest/gtest.h>
#include <simcore/robust_fp.hpp>
#include <vector>
#include <limits>

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

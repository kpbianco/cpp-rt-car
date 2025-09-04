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
    v.insert(v.end(), 1000000, 1e-16); // lots of tiny contributions

    // naive sum
    double naive = 0.0;
    for (double x : v) naive += x;

    // kahan sum
    double kahan = robust::kahan_sum(v.begin(), v.end());

    // reference using long double
    long double ref = 0.0L;
    for (double x : v) ref += static_cast<long double>(x);
    double refd = static_cast<double>(ref);

    auto naiveULP = robust::ulp_distance(naive, refd);
    auto kahanULP = robust::ulp_distance(kahan, refd);
    EXPECT_LT(kahanULP, naiveULP);
}

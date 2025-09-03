// tests/test_simd.cpp
#include <gtest/gtest.h>
#include <vector>
#include <random>
#include <cstddef>

#include <simcore/soa_simd.hpp>

TEST(SIMD, SoAAxpy3MatchesScalar)
{
    std::mt19937 rng(123);
    std::uniform_real_distribution<double> dist(-10.0, 10.0);

    for (std::size_t n : {1ul, 7ul, 13ul, 64ul, 77ul, 1024ul}) {
        std::vector<double> px(n), py(n), pz(n);
        std::vector<double> vx(n), vy(n), vz(n);

        for (std::size_t i = 0; i < n; ++i) {
            px[i] = dist(rng);  py[i] = dist(rng);  pz[i] = dist(rng);
            vx[i] = dist(rng);  vy[i] = dist(rng);  vz[i] = dist(rng);
        }

        const double a = 0.123;

        // Scalar reference
        auto px_ref = px, py_ref = py, pz_ref = pz;
        for (std::size_t i = 0; i < n; ++i) {
            px_ref[i] += a * vx[i];
            py_ref[i] += a * vy[i];
            pz_ref[i] += a * vz[i];
        }

        // SIMD/SoA path (or scalar fallback if AVX2 disabled)
        soa::Vec3SoA<double>       p{ px.data(), py.data(), pz.data() };
        soa::Vec3SoA<const double> v{ vx.data(), vy.data(), vz.data() };
        soa::axpy3(a, v, p, n);

        for (std::size_t i = 0; i < n; ++i) {
            EXPECT_DOUBLE_EQ(px_ref[i], px[i]);
            EXPECT_DOUBLE_EQ(py_ref[i], py[i]);
            EXPECT_DOUBLE_EQ(pz_ref[i], pz[i]);
        }
    }
}

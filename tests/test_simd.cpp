// tests/test_simd.cpp
#include <gtest/gtest.h>
#include <vector>
#include <random>
#include <cstddef>
#include <chrono>
#include <iostream>
#include <cmath>

#define ENABLE_SIMD 1
#include <simcore/soa/soa_simd.hpp>
#include <simcore/soa/aosoa.hpp>

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

        // Scalar reference using fused multiply-add to match SIMD semantics
        auto px_ref = px, py_ref = py, pz_ref = pz;
        for (std::size_t i = 0; i < n; ++i) {
            px_ref[i] = std::fma(a, vx[i], px_ref[i]);
            py_ref[i] = std::fma(a, vy[i], py_ref[i]);
            pz_ref[i] = std::fma(a, vz[i], pz_ref[i]);
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

TEST(SIMD, AoSoAAxpy3MatchesSoA)
{
    using Ao = soa::Vec3AoSoA<double, 256>;
    using Aoc = soa::Vec3AoSoA<const double, 256>;
    std::mt19937 rng(123);
    std::uniform_real_distribution<double> dist(-10.0, 10.0);
    const double a = 0.123;

    for (std::size_t n : {1000ul, 10000ul, 100000ul, 1000000ul}) {
        std::size_t tiles = (n + Ao::tile_size - 1) / Ao::tile_size;
        std::vector<typename Ao::Tile> pv(tiles), vv(tiles);
        Ao p{pv.data()};
        Aoc v{vv.data()};

        std::vector<double> px(n), py(n), pz(n);
        std::vector<double> vx(n), vy(n), vz(n);
        for (std::size_t i = 0; i < n; ++i) {
            double pxv = dist(rng); double pyv = dist(rng); double pzv = dist(rng);
            double vxv = dist(rng); double vyv = dist(rng); double vzv = dist(rng);
            px[i] = pxv; py[i] = pyv; pz[i] = pzv;
            vx[i] = vxv; vy[i] = vyv; vz[i] = vzv;
            std::size_t t = i / Ao::tile_size; std::size_t j = i % Ao::tile_size;
            pv[t].x[j] = pxv; pv[t].y[j] = pyv; pv[t].z[j] = pzv;
            vv[t].x[j] = vxv; vv[t].y[j] = vyv; vv[t].z[j] = vzv;
        }

        auto start_soa = std::chrono::high_resolution_clock::now();
        soa::Vec3SoA<double> psoa{ px.data(), py.data(), pz.data() };
        soa::Vec3SoA<const double> vsoa{ vx.data(), vy.data(), vz.data() };
        soa::axpy3(a, vsoa, psoa, n);
        auto end_soa = std::chrono::high_resolution_clock::now();

        auto start_aosoa = std::chrono::high_resolution_clock::now();
        soa::axpy3<Ao::tile_size>(a, v, p, n);
        auto end_aosoa = std::chrono::high_resolution_clock::now();

        auto soa_us = std::chrono::duration_cast<std::chrono::microseconds>(end_soa - start_soa).count();
        auto aosoa_us = std::chrono::duration_cast<std::chrono::microseconds>(end_aosoa - start_aosoa).count();
        std::cout << "N=" << n << " SoA=" << soa_us << "us AoSoA=" << aosoa_us << "us\n";

        for (std::size_t i = 0; i < n; ++i) {
            std::size_t t = i / Ao::tile_size; std::size_t j = i % Ao::tile_size;
            EXPECT_DOUBLE_EQ(px[i], pv[t].x[j]);
            EXPECT_DOUBLE_EQ(py[i], pv[t].y[j]);
            EXPECT_DOUBLE_EQ(pz[i], pv[t].z[j]);
        }
    }
}

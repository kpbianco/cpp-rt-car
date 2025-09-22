#define ENABLE_SIMD 1
#include <simcore/soa/soa_simd.hpp>
#include <simcore/soa/aosoa.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

struct BenchResult {
    std::size_t n;
    int inner;
    double soa_us;
    double aosoa_us;
    double ratio;
};

static int pick_inner_loops(std::size_t n) {
    if (n <= 1'000)
        return 4'000;
    if (n <= 10'000)
        return 1'200;
    if (n <= 100'000)
        return 320;
    return 80;
}

int main(int argc, char** argv) {
    int repeats = 5;
    bool assertSpeedup = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeats = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--assert-speedup") == 0) {
            assertSpeedup = true;
        }
    }

    using Clock = std::chrono::steady_clock;
    using AoSoA = soa::Vec3AoSoA<double, 256>;
    using AoSoAConst = soa::Vec3AoSoA<const double, 256>;
    using Tile = AoSoA::Tile;

    const std::vector<std::size_t> sizes = {1'000u, 10'000u, 100'000u, 1'000'000u};
    const double scale = 0.016;

    std::mt19937 rng(1337);
    std::uniform_real_distribution<double> dist(-5.0, 5.0);

    std::vector<BenchResult> results;
    results.reserve(sizes.size());

    for (std::size_t n : sizes) {
        const int inner = pick_inner_loops(n);

        std::vector<double> basePx(n), basePy(n), basePz(n);
        std::vector<double> baseVx(n), baseVy(n), baseVz(n);
        for (std::size_t i = 0; i < n; ++i) {
            basePx[i] = dist(rng);
            basePy[i] = dist(rng);
            basePz[i] = dist(rng);
            baseVx[i] = dist(rng);
            baseVy[i] = dist(rng);
            baseVz[i] = dist(rng);
        }

        const std::size_t tileCount = (n + AoSoA::tile_size - 1) / AoSoA::tile_size;
        const std::size_t totalSlots = tileCount * AoSoA::tile_size;

        auto fill_tiles = [&](std::vector<Tile>& tiles, const std::vector<double>& x,
                              const std::vector<double>& y, const std::vector<double>& z) {
            tiles.resize(tileCount);
            for (std::size_t idx = 0; idx < totalSlots; ++idx) {
                const std::size_t tile = idx / AoSoA::tile_size;
                const std::size_t lane = idx % AoSoA::tile_size;
                const double vx = (idx < x.size()) ? x[idx] : 0.0;
                const double vy = (idx < y.size()) ? y[idx] : 0.0;
                const double vz = (idx < z.size()) ? z[idx] : 0.0;
                tiles[tile].x[lane] = vx;
                tiles[tile].y[lane] = vy;
                tiles[tile].z[lane] = vz;
            }
        };

        std::vector<Tile> velocityTiles;
        std::vector<Tile> positionTilesBase;
        fill_tiles(velocityTiles, baseVx, baseVy, baseVz);
        fill_tiles(positionTilesBase, basePx, basePy, basePz);
        AoSoAConst velocityAo{velocityTiles.data()};

        double bestSoa = std::numeric_limits<double>::max();
        double bestAoSoa = std::numeric_limits<double>::max();

        for (int rep = 0; rep < repeats; ++rep) {
            auto soaPx = basePx;
            auto soaPy = basePy;
            auto soaPz = basePz;
            soa::Vec3SoA<double> soaPos{soaPx.data(), soaPy.data(), soaPz.data()};
            soa::Vec3SoA<const double> soaVel{baseVx.data(), baseVy.data(), baseVz.data()};

            auto startSoa = Clock::now();
            for (int iter = 0; iter < inner; ++iter)
                soa::axpy3(scale, soaVel, soaPos, n);
            auto endSoa = Clock::now();
            double soaUs = std::chrono::duration<double, std::micro>(endSoa - startSoa).count();
            soaUs /= static_cast<double>(inner);
            bestSoa = std::min(bestSoa, soaUs);

            auto positionTiles = positionTilesBase;
            AoSoA positionAo{positionTiles.data()};
            auto startAo = Clock::now();
            for (int iter = 0; iter < inner; ++iter)
                soa::axpy3<AoSoA::tile_size>(scale, velocityAo, positionAo, n);
            auto endAo = Clock::now();
            double aoUs = std::chrono::duration<double, std::micro>(endAo - startAo).count();
            aoUs /= static_cast<double>(inner);
            bestAoSoa = std::min(bestAoSoa, aoUs);
        }

        BenchResult res{};
        res.n = n;
        res.inner = inner;
        res.soa_us = bestSoa;
        res.aosoa_us = bestAoSoa;
        res.ratio = bestSoa / bestAoSoa;
        results.push_back(res);
    }

    bool speedupOk = true;
    if (assertSpeedup && !results.empty()) {
        const BenchResult& last = results.back();
        speedupOk = last.aosoa_us <= last.soa_us;
        if (!speedupOk)
            std::cerr << "AoSoA slower than SoA for N=" << last.n << "\n";
    }

    std::cout.setf(std::ios::fixed);
    std::cout << std::setprecision(3);
    std::cout << "{\n";
    std::cout << "  \"benchmark\": \"aosoa_vs_soa_axpy3\",\n";
    std::cout << "  \"repeats\": " << repeats << ",\n";
    std::cout << "  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::cout << "    {\"n\": " << r.n
                  << ", \"inner\": " << r.inner
                  << ", \"soa_us\": " << r.soa_us
                  << ", \"aosoa_us\": " << r.aosoa_us
                  << ", \"ratio\": " << r.ratio << "}";
        if (i + 1 < results.size())
            std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "  ],\n";
    std::cout << "  \"assert_speedup\": " << (assertSpeedup ? "true" : "false");
    if (assertSpeedup)
        std::cout << ",\n  \"speedup_ok\": " << (speedupOk ? "true" : "false") << "\n";
    else
        std::cout << "\n";
    std::cout << "}\n";

    if (assertSpeedup && !speedupOk)
        return 1;
    return 0;
}

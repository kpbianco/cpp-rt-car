#include <simcore/SimCore.hpp>
#include <simcore/worker_pool.hpp>
#include <simcore/soa/aosoa.hpp>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>
#include <cmath>

int main() {
    using ParticleAoSoA = soa::Vec3AoSoA<double, 256>;
    constexpr std::size_t kParticleCount = 8192;
    const std::size_t tileCount = (kParticleCount + ParticleAoSoA::tile_size - 1) / ParticleAoSoA::tile_size;

    std::vector<ParticleAoSoA::Tile> positionTiles(tileCount);
    std::vector<ParticleAoSoA::Tile> velocityTiles(tileCount);

    for (auto &tile : positionTiles) {
        std::fill_n(tile.x, ParticleAoSoA::tile_size, 0.0);
        std::fill_n(tile.y, ParticleAoSoA::tile_size, 0.0);
        std::fill_n(tile.z, ParticleAoSoA::tile_size, 0.0);
    }
    for (auto &tile : velocityTiles) {
        std::fill_n(tile.x, ParticleAoSoA::tile_size, 0.0);
        std::fill_n(tile.y, ParticleAoSoA::tile_size, 0.0);
        std::fill_n(tile.z, ParticleAoSoA::tile_size, 0.0);
    }

    ParticleAoSoA positions{positionTiles.data()};
    ParticleAoSoA velocities{velocityTiles.data()};

    for (std::size_t idx = 0; idx < kParticleCount; ++idx) {
        const std::size_t tile = idx / ParticleAoSoA::tile_size;
        const std::size_t lane = idx % ParticleAoSoA::tile_size;
        positions.tiles[tile].x[lane] = static_cast<double>(idx) * 0.02;
        positions.tiles[tile].y[lane] = 10.0;
        positions.tiles[tile].z[lane] = 0.0;
        velocities.tiles[tile].x[lane] = 1.0;
        velocities.tiles[tile].y[lane] = 0.0;
        velocities.tiles[tile].z[lane] = 0.0;
    }

    unsigned hw = std::thread::hardware_concurrency();
    if (hw < 2)
        hw = 2;
    SimCore::Settings settings;
    settings.hz = 120.0;
    settings.maxFrames = 240;
    settings.threads = hw;
    settings.autoTuneChunks = false;
    settings.chunkSize = 4;

    WorkerPool pool(settings.threads, 2048);
    SimCore sim(settings);
    sim.setWorkerPool(&pool);

    auto physics = sim.addPhase("AoSoA physics");
    sim.setPhaseElementCount(physics, tileCount);

    constexpr double gravity = -9.81;
    constexpr double restitution = 0.35;

    sim.addParallelRangeTask(
        physics,
        [positions, velocities, gravity, restitution, particleCount = kParticleCount](std::size_t begin, std::size_t end, std::int64_t,
                                                                                      SimCore::Seconds dt) {
            const double g = gravity;
            const double bounce = restitution;
            const double step = dt.count();
            for (std::size_t tileIndex = begin; tileIndex < end; ++tileIndex) {
                auto &posTile = positions.tiles[tileIndex];
                auto &velTile = velocities.tiles[tileIndex];
                for (std::size_t lane = 0; lane < ParticleAoSoA::tile_size; ++lane) {
                    const std::size_t idx = tileIndex * ParticleAoSoA::tile_size + lane;
                    if (idx >= particleCount)
                        break;

                    velTile.y[lane] += g * step;
                    posTile.x[lane] += velTile.x[lane] * step;
                    posTile.y[lane] += velTile.y[lane] * step;
                    posTile.z[lane] += velTile.z[lane] * step;

                    if (posTile.y[lane] < 0.0) {
                        posTile.y[lane] = 0.0;
                        velTile.y[lane] = -velTile.y[lane] * bounce;
                        if (std::abs(velTile.y[lane]) < 0.05)
                            velTile.y[lane] = 0.0;
                    }
                }
            }
        });

    sim.run();
    pool.drain();
    pool.stop();
    sim.setWorkerPool(nullptr);

    double avgHeight = 0.0;
    for (std::size_t idx = 0; idx < kParticleCount; ++idx) {
        const std::size_t tile = idx / ParticleAoSoA::tile_size;
        const std::size_t lane = idx % ParticleAoSoA::tile_size;
        avgHeight += positions.tiles[tile].y[lane];
    }
    avgHeight /= static_cast<double>(kParticleCount);

    std::cout << "Particles settled. First particle y=" << positions.tiles[0].y[0]
              << ", average height=" << avgHeight << "\n";
    return 0;
}

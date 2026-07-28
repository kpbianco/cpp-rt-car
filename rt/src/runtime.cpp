#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

#include "rt/runtime.hpp"

#include <simcore/SimCore.hpp>

#include "../../hal/gpu_stub.hpp"

namespace {

void busy_spin(double milliseconds) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto target = std::chrono::duration<double, std::milli>(milliseconds);
    while (Clock::now() - start < target) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
}

} // namespace

namespace rt {

DemoPipeline build_demo_pipeline(SimCore& sim) {
    auto state = std::make_shared<DemoPipeline::State>();

    const std::size_t ingest = sim.addPhase("demo.ingest", 256);
    const std::size_t lighting = sim.addPhase("demo.lighting");
    const std::size_t io = sim.addPhase("demo.io");
    const std::size_t compose = sim.addPhase("demo.compose");

    sim.addParallelRangeTask(
        ingest,
        [](std::size_t, std::size_t, std::int64_t, SimCore::Seconds) {
            busy_spin(0.04);
        });

    sim.addSerialSubsystem(ingest, [state](std::int64_t, SimCore::Seconds) {
        state->ingestCount.fetch_add(1, std::memory_order_relaxed);
        busy_spin(0.55);
    });

    sim.addSerialSubsystem(lighting, [state, &sim](std::int64_t, SimCore::Seconds) {
        state->gpuCount.fetch_add(1, std::memory_order_relaxed);
        busy_spin(0.22);
        auto fenceA = simcore::hal::gpu::submit([] { busy_spin(0.32); });
        auto fenceB = simcore::hal::gpu::submit([] { busy_spin(0.22); });
        const auto gpuIndex = state->gpuCount.load(std::memory_order_relaxed);
        sim.bintrace().log(bintrace::EV_GpuFenceWaitBegin, 2u,
                           static_cast<std::uint64_t>(gpuIndex));
        auto &fiberPool = sim.fiberPool();
        fiberPool.wait_for_fence(std::move(fenceA));
        fiberPool.wait_for_fence(std::move(fenceB));
        fiberPool.drain();
        sim.bintrace().log(bintrace::EV_GpuFenceWaitEnd, 2u,
                           static_cast<std::uint64_t>(gpuIndex));
        state->fenceWaits.fetch_add(1, std::memory_order_relaxed);
        busy_spin(0.13);
    });

    sim.addSerialSubsystem(io, [state](std::int64_t, SimCore::Seconds) {
        state->ioCount.fetch_add(1, std::memory_order_relaxed);
        busy_spin(0.70);
    });

    sim.addSerialSubsystem(compose, [state, &sim](std::int64_t frame,
                                                  SimCore::Seconds) {
        state->composeCount.fetch_add(1, std::memory_order_relaxed);
        for (int rung = 1; rung <= 4; ++rung) {
            auto &seen = state->rungEventsSeen[static_cast<std::size_t>(rung - 1)];
            auto observed = seen.load(std::memory_order_relaxed);
            const auto current = sim.rungActivations(rung);
            while (observed < current) {
                if (seen.compare_exchange_weak(observed, observed + 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
                    const std::uint64_t frameTag =
                        frame > 0 ? static_cast<std::uint64_t>(frame - 1) : 0ull;
                    sim.bintrace().log(bintrace::EV_BudgetLadder,
                                       static_cast<std::uint32_t>(rung), frameTag);
                    ++observed;
                }
            }
        }
        busy_spin(0.60);
    });

    sim.addDependency(ingest, compose);
    sim.addDependency(lighting, compose);
    sim.addDependency(io, compose);

    return DemoPipeline(std::move(state));
}

} // namespace rt

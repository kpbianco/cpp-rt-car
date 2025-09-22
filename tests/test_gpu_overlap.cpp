#include <gtest/gtest.h>
#include <gpu/frame_graph.hpp>
#include <simcore/bintrace.hpp>
#include <simcore/hal.hpp>

#include <chrono>
#include <thread>

using simcore::hal::gpu::FrameGraph;

TEST(GPUOverlap, FiberFenceAwaitYieldsWork) {
    using namespace std::chrono_literals;

    bintrace::Trace trace;
    trace.init(/*threads=*/1, /*eventsPerThread=*/128, /*enabled=*/true);
    trace.bindThread(0);
    bintrace::set_global_trace(&trace);

    FrameGraph fg;
    constexpr auto gpu_time = 80ms;
    constexpr auto cpu_time = 60ms;
    constexpr auto epsilon = 20ms;
    constexpr auto tolerance = 15ms;

    fg.add_pass(
        []() {},
        [&]() {
            std::this_thread::sleep_for(gpu_time);
        });

    fg.add_pass(
        [&]() {
            std::this_thread::sleep_for(cpu_time);
        },
        FrameGraph::PassFn{}
    );

    auto start = simcore::hal::now();
    auto budget = fg.execute();
    auto total = simcore::hal::elapsed(start, simcore::hal::now());
    auto overlap = fg.overlap();

    auto snapshot = trace.snapshot();
    bintrace::set_global_trace(nullptr);
    trace.shutdown();

    auto cpu_ms = std::chrono::duration<double, std::milli>(budget.cpu).count();
    auto gpu_ms = std::chrono::duration<double, std::milli>(budget.gpu).count();
    auto total_ms = std::chrono::duration<double, std::milli>(total).count();
    auto overlap_ms = std::chrono::duration<double, std::milli>(overlap).count();

    EXPECT_GE(cpu_ms, static_cast<double>((cpu_time - tolerance).count()));
    EXPECT_GE(gpu_ms, static_cast<double>((gpu_time - tolerance).count()));
    EXPECT_LT(total_ms, static_cast<double>((cpu_time + gpu_time - epsilon).count()));
    EXPECT_GE(overlap_ms, static_cast<double>((cpu_time - tolerance).count()));

    std::size_t fenceBegin = 0;
    std::size_t fenceEnd = 0;
    for (const auto& ev : snapshot.events) {
        if (ev.code == bintrace::EV_GpuFenceWaitBegin) {
            ++fenceBegin;
        } else if (ev.code == bintrace::EV_GpuFenceWaitEnd) {
            ++fenceEnd;
        }
    }
    EXPECT_GE(fenceBegin, 1u);
    EXPECT_GE(fenceEnd, 1u);
    EXPECT_EQ(fenceBegin, fenceEnd);
}

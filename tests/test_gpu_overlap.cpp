#include <gtest/gtest.h>
#include <gpu/frame_graph.hpp>
#include <simcore/hal.hpp>

#include <chrono>
#include <thread>

using simcore::hal::gpu::FrameGraph;

TEST(GPUOverlap, FiberFenceAwaitYieldsWork) {
    using namespace std::chrono_literals;

    FrameGraph fg;
    constexpr auto gpu_time = 80ms;
    constexpr auto cpu_time = 60ms;

    fg.add_pass(
        []() {},
        [&]() {
            std::this_thread::sleep_for(gpu_time);
        });

    fg.add_pass(
        [&]() {
            auto start = simcore::hal::now();
            while (simcore::hal::elapsed(start, simcore::hal::now()) < cpu_time) {
                std::this_thread::yield();
            }
        },
        FrameGraph::PassFn{}
    );

    auto start = simcore::hal::now();
    auto budget = fg.execute();
    auto total = simcore::hal::elapsed(start, simcore::hal::now());

    auto cpu_ms = std::chrono::duration_cast<std::chrono::milliseconds>(budget.cpu).count();
    auto gpu_ms = std::chrono::duration_cast<std::chrono::milliseconds>(budget.gpu).count();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total).count();
    auto overlap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fg.overlap()).count();

    EXPECT_GE(cpu_ms, cpu_time.count() - 10);
    EXPECT_GE(gpu_ms, gpu_time.count() - 10);
    EXPECT_LT(total_ms, cpu_ms + gpu_ms);
    EXPECT_GT(overlap_ms, 0);
}

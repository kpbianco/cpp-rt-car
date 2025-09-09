#include <gtest/gtest.h>
#include <gpu/frame_graph.hpp>
#include <atomic>

using namespace std::chrono_literals;

using simcore::hal::gpu::FrameGraph;

TEST(FrameGraph, TimelineAndResources) {
    FrameGraph fg;
    auto res_idx = fg.create_resource(sizeof(int));
    auto& res = fg.resource(res_idx);
    auto ptr = static_cast<int*>(res.buf.data);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 64u, 0u);

    std::atomic<int> gpu_seen{0};
    fg.add_pass([&]() { *ptr = 7; },
                [&]() { gpu_seen.store(*ptr, std::memory_order_release); },
                {res_idx});

    auto budget = fg.execute();

    EXPECT_EQ(gpu_seen.load(std::memory_order_acquire), 7);
    EXPECT_EQ(fg.cpu_timeline().current(), 1u);
    EXPECT_EQ(fg.gpu_timeline().current(), 1u);
    EXPECT_GT(budget.cpu.count(), 0);
    EXPECT_GT(budget.gpu.count(), 0);
    EXPECT_GT(fg.overlap().count(), 0);
    EXPECT_EQ(fg.resource(res_idx).buf.data, nullptr);
}

TEST(FrameGraph, MixedComputeStubs) {
    auto fence = simcore::hal::gpu::submit_spirv(nullptr, 0, []() {});
    EXPECT_TRUE(simcore::hal::gpu::fence_wait(fence, 100ms));
    auto fence2 = simcore::hal::gpu::submit_cuda([]() {});
    EXPECT_TRUE(simcore::hal::gpu::fence_wait(fence2, 100ms));
}


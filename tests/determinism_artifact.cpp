#include <rt/runtime.hpp>
#include <rt/snapshot.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <vector>

namespace {

using CanonicalU64 = std::array<std::byte, sizeof(std::uint64_t)>;

struct ArtifactWorkload {
    CanonicalU64 input{};
    CanonicalU64 accumulator{};
};

rt::CallbackResult update(
    void* opaque,
    const rt::CallbackContext& context) {
    auto& state = *static_cast<ArtifactWorkload*>(opaque);
    std::uint64_t input = 0;
    std::uint64_t accumulator = 0;
    if (!rt::load_u64_le(state.input, 0, input) ||
        !rt::load_u64_le(
            state.accumulator,
            0,
            accumulator) ||
        !rt::store_u64_le(
            state.accumulator,
            0,
            accumulator * 6364136223846793005ull +
                input +
                context.frame.frame_index)) {
        return rt::CallbackResult::error;
    }
    return rt::CallbackResult::ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || !argv[1]) {
        return 2;
    }

    ArtifactWorkload state;
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.scratch_bytes = 64;
    config.trace_capacity = 16;
    config.worker_count = 1;
    config.executor_queue_capacity = 8;
    config.task_scratch_bytes = 64;
    config.task_scratch_slots = 8;
    config.memory_budget_bytes = 1024 * 1024;
    config.determinism_tier =
        rt::DeterminismTier::schedule_independent;
    config.state_capacity = 2;
    config.snapshot_max_bytes = 2048;
    config.replay_input_capacity = 64;
    config.input_log_max_bytes = 4096;
    if (runtime.configure(config) != rt::Status::ok ||
        runtime.register_callback(
            {"artifact.update", &update, &state}) !=
            rt::Status::ok ||
        runtime.register_state(
            {"artifact.input", 1, state.input}) !=
            rt::Status::ok ||
        runtime.register_state(
            {"artifact.accumulator", 1, state.accumulator}) !=
            rt::Status::ok ||
        runtime.finalize() != rt::Status::ok ||
        runtime.start() != rt::Status::ok) {
        return 3;
    }

    for (std::uint64_t frame = 0; frame < 64; ++frame) {
        if (!rt::store_u64_le(
                state.input,
                0,
                frame * 37u + 11u) ||
            runtime.step(rt::HostFrameContext{
                frame,
                std::chrono::nanoseconds(1'000'000),
                std::nullopt,
            }) != rt::Status::ok) {
            return 4;
        }
    }

    std::size_t required = 0;
    if (runtime.checkpoint_size(required) != rt::Status::ok) {
        return 5;
    }
    std::vector<std::byte> artifact(required);
    rt::ArtifactWriteResult result;
    if (runtime.write_checkpoint(
            63,
            artifact,
            result) != rt::Status::ok ||
        result.bytes_written != artifact.size()) {
        return 6;
    }

    std::ofstream output(
        argv[1],
        std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(artifact.data()),
        static_cast<std::streamsize>(artifact.size()));
    if (!output) {
        return 7;
    }
    return runtime.stop() == rt::Status::ok ? 0 : 8;
}

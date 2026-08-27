#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <rt/runtime.hpp>

namespace {

bool load_u64(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::uint64_t& value) {
    value = 0;
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(
            static_cast<std::uint8_t>(bytes[offset + index])) << (8u * index);
    }
    return true;
}

bool store_u64(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint64_t value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] =
            static_cast<std::byte>(value >> (8u * index));
    }
    return true;
}

struct ManualClock final : rt::RuntimeClock {
    std::uint64_t now = 1'000;
    std::uint64_t now_ns() noexcept override { return now; }
    rt::Status sleep_until_ns(std::uint64_t release) noexcept override {
        now = release;
        return rt::Status::ok;
    }
    bool supports_absolute_sleep() const noexcept override { return true; }
};

rt::CallbackResult increment_state(
    void* data,
    const rt::CallbackContext& context) {
    if (!context.rate_release) {
        return rt::CallbackResult::error;
    }
    auto bytes = std::span<std::byte>(
        static_cast<std::byte*>(data), sizeof(std::uint64_t));
    std::uint64_t value = 0;
    if (!load_u64(bytes, 0, value) ||
        !store_u64(bytes, 0, value + 1)) {
        return rt::CallbackResult::error;
    }
    return rt::CallbackResult::ok;
}

struct InputProbe {
    std::size_t calls = 0;
};

rt::CallbackResult apply_input(
    void* data,
    const rt::ReplayInputView& input) {
    auto& probe = *static_cast<InputProbe*>(data);
    if (!input.frame.nominal_release_ns || !input.payload.empty()) {
        return rt::CallbackResult::error;
    }
    ++probe.calls;
    return rt::CallbackResult::ok;
}

rt::RuntimeConfig replay_config() {
    rt::RuntimeConfig config;
    config.callback_capacity = 4;
    config.executor_queue_capacity = 4;
    config.task_scratch_slots = 4;
    config.state_capacity = 4;
    config.snapshot_max_bytes = 64 * 1024;
    config.memory_budget_bytes = 4 * 1024 * 1024;
    return config;
}

} // namespace

TEST(MixedRateReplay, ActiveArtifactRoundTripsAndDrivesTranscriptDecisions) {
    using namespace std::chrono_literals;

    ManualClock clock;
    rt::Runtime runtime(clock);
    ASSERT_EQ(runtime.configure(replay_config()), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_rate_execution_policy({8, 3, 1, 1, 8}),
        rt::Status::ok);
    const rt::MixedRateClosurePolicy closure{
        11,
        16,
        16,
        64 * 1024,
        8,
        rt::MixedRateOverflowPolicy::overwrite_committed,
        true,
        true,
        {},
    };
    ASSERT_EQ(
        runtime.set_mixed_rate_closure_policy(closure),
        rt::Status::ok);

    std::array<std::byte, 8> state{};
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(
        runtime.register_callback(
            {"counter", &increment_state, state.data()},
            phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"rate", 100, 1, 100, 10},
            domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(phase, domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_state({"state.counter", 1, state}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    std::size_t checkpoint_bytes = 0;
    ASSERT_EQ(runtime.checkpoint_size(checkpoint_bytes), rt::Status::ok);
    std::vector<std::byte> checkpoint(checkpoint_bytes);
    rt::ArtifactWriteResult checkpoint_result;
    ASSERT_EQ(
        runtime.write_checkpoint(0, checkpoint, checkpoint_result),
        rt::Status::ok);
    checkpoint.resize(checkpoint_result.bytes_written);

    const std::array frames{
        rt::HostFrameContext{1, 100ns, std::nullopt, std::uint64_t{1'000}},
        rt::HostFrameContext{2, 100ns, std::nullopt, std::uint64_t{1'100}},
    };
    for (const auto& frame : frames) {
        ASSERT_EQ(runtime.step(frame), rt::Status::ok);
    }
    std::uint64_t state_value = 0;
    ASSERT_TRUE(load_u64(state, 0, state_value));
    ASSERT_EQ(state_value, 2u);

    const std::array inputs{
        rt::ReplayInputRecord{frames[0], 1, {}},
        rt::ReplayInputRecord{frames[1], 1, {}},
    };
    std::vector<std::byte> artifact(64 * 1024);
    rt::ArtifactWriteResult artifact_result;
    ASSERT_EQ(
        runtime.write_active_replay_artifact(
            checkpoint, inputs, artifact, artifact_result),
        rt::Status::ok);
    artifact.resize(artifact_result.bytes_written);

    rt::ActiveReplayMetadata metadata;
    ASSERT_EQ(
        rt::inspect_active_replay_artifact(artifact, metadata),
        rt::Status::ok);
    EXPECT_EQ(metadata.schema_version, 1u);
    EXPECT_EQ(metadata.input_record_count, inputs.size());
    EXPECT_EQ(metadata.action_record_count, 2u);
    EXPECT_EQ(metadata.first_action_sequence, 0u);
    EXPECT_EQ(metadata.last_action_sequence, 1u);
    EXPECT_EQ(metadata.first_frame_index, 1u);
    EXPECT_EQ(metadata.last_frame_index, 2u);

    InputProbe input_probe;
    rt::ActiveReplayResult replay_result;
    ASSERT_EQ(
        runtime.replay_active(
            artifact, &apply_input, &input_probe, &replay_result),
        rt::Status::ok);
    EXPECT_EQ(input_probe.calls, 2u);
    EXPECT_EQ(replay_result.replay.records_processed, 2u);
    EXPECT_EQ(replay_result.replay.frames_replayed, 2u);
    EXPECT_EQ(replay_result.actions_compared, 2u);
    EXPECT_EQ(replay_result.mismatch_status, rt::Status::ok);
    ASSERT_TRUE(load_u64(state, 0, state_value));
    EXPECT_EQ(state_value, 2u);

    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MixedRateReplay, ParserRejectsMutationTrailingBytesAndShortOutput) {
    using namespace std::chrono_literals;

    ManualClock clock;
    rt::Runtime runtime(clock);
    ASSERT_EQ(runtime.configure(replay_config()), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_mixed_rate_closure_policy({
            5,
            8,
            8,
            32 * 1024,
            4,
            rt::MixedRateOverflowPolicy::overwrite_committed,
            true,
            true,
            {},
        }),
        rt::Status::ok);
    std::array<std::byte, 8> state{};
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(
        runtime.register_callback(
            {"counter", &increment_state, state.data()}, phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"rate", 100, 1, 100, 10}, domain),
        rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    ASSERT_EQ(runtime.register_state({"counter", 1, state}), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    std::size_t checkpoint_bytes = 0;
    ASSERT_EQ(runtime.checkpoint_size(checkpoint_bytes), rt::Status::ok);
    std::vector<std::byte> checkpoint(checkpoint_bytes);
    rt::ArtifactWriteResult checkpoint_result;
    ASSERT_EQ(
        runtime.write_checkpoint(0, checkpoint, checkpoint_result),
        rt::Status::ok);
    checkpoint.resize(checkpoint_result.bytes_written);
    const rt::HostFrameContext frame{
        1, 100ns, std::nullopt, std::uint64_t{1'000}};
    ASSERT_EQ(runtime.step(frame), rt::Status::ok);
    const std::array inputs{rt::ReplayInputRecord{frame, 1, {}}};

    std::array<std::byte, 1> short_output{};
    rt::ArtifactWriteResult short_result;
    EXPECT_EQ(
        runtime.write_active_replay_artifact(
            checkpoint, inputs, short_output, short_result),
        rt::Status::capacity_exceeded);
    EXPECT_GT(short_result.required_bytes, short_output.size());
    EXPECT_EQ(short_result.bytes_written, 0u);

    std::vector<std::byte> artifact(short_result.required_bytes);
    rt::ArtifactWriteResult result;
    ASSERT_EQ(
        runtime.write_active_replay_artifact(
            checkpoint, inputs, artifact, result),
        rt::Status::ok);
    artifact.resize(result.bytes_written);

    auto mutated = artifact;
    mutated[mutated.size() / 2] ^= std::byte{1};
    rt::ActiveReplayMetadata metadata;
    EXPECT_EQ(
        rt::inspect_active_replay_artifact(mutated, metadata),
        rt::Status::invalid_artifact);
    std::uint64_t state_before_rejection = 0;
    ASSERT_TRUE(load_u64(state, 0, state_before_rejection));
    InputProbe rejection_probe;
    rt::ActiveReplayResult rejection_result;
    EXPECT_EQ(
        runtime.replay_active(
            mutated,
            &apply_input,
            &rejection_probe,
            &rejection_result),
        rt::Status::invalid_artifact);
    std::uint64_t state_after_rejection = 0;
    ASSERT_TRUE(load_u64(state, 0, state_after_rejection));
    EXPECT_EQ(state_after_rejection, state_before_rejection);
    EXPECT_EQ(rejection_probe.calls, 0u);

    auto trailing = artifact;
    trailing.push_back(std::byte{0});
    EXPECT_EQ(
        rt::inspect_active_replay_artifact(trailing, metadata),
        rt::Status::invalid_artifact);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

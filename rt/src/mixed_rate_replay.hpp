#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <rt/runtime.hpp>

namespace rt::detail {

inline constexpr std::size_t active_replay_header_size = 384;
inline constexpr std::size_t active_replay_input_header_size = 80;
inline constexpr std::size_t active_replay_action_bytes =
    sizeof(MixedRateActionRecord) + sizeof(std::uint64_t);

using MixedRateActionReader = bool (*)(
    void* context,
    std::uint64_t sequence,
    MixedRateActionRecord& record) noexcept;

struct ActiveReplayArtifactView {
    ActiveReplayMetadata metadata{};
    std::span<const std::byte> artifact{};
    std::span<const std::byte> checkpoint{};
    std::size_t input_offset = 0;
    std::size_t action_offset = 0;
};

[[nodiscard]] Status encode_active_replay_artifact(
    ActiveReplayMetadata metadata,
    std::span<const std::byte> checkpoint,
    std::span<const ReplayInputRecord> inputs,
    std::uint64_t first_action_sequence,
    std::size_t action_count,
    MixedRateActionReader reader,
    void* reader_context,
    std::size_t maximum_bytes,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept;

[[nodiscard]] Status parse_active_replay_artifact(
    std::span<const std::byte> artifact,
    std::size_t maximum_bytes,
    ActiveReplayArtifactView& view) noexcept;

[[nodiscard]] bool active_replay_input_at(
    const ActiveReplayArtifactView& view,
    std::size_t index,
    ReplayInputView& input) noexcept;

[[nodiscard]] bool active_replay_action_at(
    const ActiveReplayArtifactView& view,
    std::size_t index,
    MixedRateActionRecord& action) noexcept;

} // namespace rt::detail

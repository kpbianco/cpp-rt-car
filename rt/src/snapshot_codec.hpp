#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <rt/runtime.hpp>

namespace rt::detail {

inline constexpr std::size_t checkpoint_header_size = 256;
inline constexpr std::size_t checkpoint_record_header_size = 88;
inline constexpr std::size_t input_log_header_size = 192;
inline constexpr std::size_t input_log_record_header_size = 48;
inline constexpr std::size_t artifact_absolute_max_bytes =
    std::size_t{1} << 30;
inline constexpr std::size_t artifact_absolute_max_records =
    std::size_t{1} << 20;

struct StateWriteView {
    std::string_view name{};
    std::uint32_t schema_version = 0;
    std::span<const std::byte> payload{};
};

using StateWriteProvider = bool (*)(
    void* context,
    std::size_t index,
    StateWriteView& state) noexcept;

struct CheckpointRecordView {
    std::string_view name{};
    std::uint32_t schema_version = 0;
    std::span<const std::byte> payload{};
};

struct CheckpointRecordCursor {
    std::size_t offset = checkpoint_header_size;
    std::uint32_t index = 0;
};

struct InputLogRecordView {
    HostFrameContext frame{};
    std::uint32_t input_type = 0;
    std::span<const std::byte> payload{};
};

struct InputLogRecordCursor {
    std::size_t offset = input_log_header_size;
    std::uint32_t index = 0;
};

[[nodiscard]] bool checked_artifact_add(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept;
[[nodiscard]] bool checked_artifact_multiply(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept;

[[nodiscard]] std::uint64_t artifact_checksum(
    std::span<const std::byte> bytes) noexcept;

[[nodiscard]] Status encode_checkpoint_artifact(
    CheckpointMetadata metadata,
    std::size_t state_count,
    StateWriteProvider provider,
    void* provider_context,
    std::size_t max_bytes,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept;

[[nodiscard]] Status parse_checkpoint_artifact(
    std::span<const std::byte> artifact,
    std::size_t max_bytes,
    std::size_t max_states,
    CheckpointMetadata& metadata) noexcept;

[[nodiscard]] bool next_checkpoint_record(
    std::span<const std::byte> artifact,
    const CheckpointMetadata& metadata,
    CheckpointRecordCursor& cursor,
    CheckpointRecordView& record) noexcept;

[[nodiscard]] Status encode_input_log_artifact(
    InputLogMetadata metadata,
    std::span<const ReplayInputRecord> records,
    std::size_t max_records,
    std::size_t max_bytes,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept;

[[nodiscard]] Status parse_input_log_artifact(
    std::span<const std::byte> artifact,
    std::size_t max_bytes,
    std::size_t max_records,
    InputLogMetadata& metadata) noexcept;

[[nodiscard]] bool next_input_log_record(
    std::span<const std::byte> artifact,
    const InputLogMetadata& metadata,
    InputLogRecordCursor& cursor,
    InputLogRecordView& record) noexcept;

} // namespace rt::detail

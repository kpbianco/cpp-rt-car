#include "snapshot_codec.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>

#include <rt/snapshot.hpp>

namespace {

constexpr std::array<std::byte, 8> kCheckpointMagic{
    std::byte{'R'}, std::byte{'T'}, std::byte{'F'}, std::byte{'W'},
    std::byte{'C'}, std::byte{'P'}, std::byte{'1'}, std::byte{0}};
constexpr std::array<std::byte, 8> kInputLogMagic{
    std::byte{'R'}, std::byte{'T'}, std::byte{'F'}, std::byte{'W'},
    std::byte{'I'}, std::byte{'L'}, std::byte{'1'}, std::byte{0}};

constexpr std::size_t kCheckpointChecksumOffset = 112;
constexpr std::size_t kInputLogChecksumOffset = 80;
constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ull;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ull;

bool identifier_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '.' || value == '_' || value == ':' ||
           value == '/' || value == '@' || value == '-';
}

bool valid_identifier(std::string_view value) noexcept {
    if (value.empty() ||
        value.size() >= rt::replay_identifier_capacity) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [](char character) {
            return identifier_character(character);
        });
}

bool fixed_identifier(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::array<char, rt::replay_identifier_capacity>& output) noexcept {
    if (offset > bytes.size() ||
        bytes.size() - offset < output.size()) {
        return false;
    }
    std::size_t length = 0;
    for (; length < output.size(); ++length) {
        output[length] = static_cast<char>(
            static_cast<std::uint8_t>(bytes[offset + length]));
        if (output[length] == '\0') {
            break;
        }
        if (!identifier_character(output[length])) {
            return false;
        }
    }
    if (length == 0 || length == output.size()) {
        return false;
    }
    for (std::size_t index = length + 1;
         index < output.size();
         ++index) {
        output[index] = static_cast<char>(
            static_cast<std::uint8_t>(bytes[offset + index]));
        if (output[index] != '\0') {
            return false;
        }
    }
    return true;
}

bool fixed_name(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::string_view& output) noexcept {
    if (offset > bytes.size() ||
        bytes.size() - offset < rt::replay_identifier_capacity) {
        return false;
    }
    std::size_t length = 0;
    for (; length < rt::replay_identifier_capacity; ++length) {
        const auto character = static_cast<char>(
            static_cast<std::uint8_t>(bytes[offset + length]));
        if (character == '\0') {
            break;
        }
        if (!identifier_character(character)) {
            return false;
        }
    }
    if (length == 0 || length == rt::replay_identifier_capacity) {
        return false;
    }
    for (std::size_t index = length + 1;
         index < rt::replay_identifier_capacity;
         ++index) {
        if (bytes[offset + index] != std::byte{0}) {
            return false;
        }
    }
    output = std::string_view(
        reinterpret_cast<const char*>(bytes.data() + offset),
        length);
    return true;
}

void write_identifier(
    std::span<std::byte> output,
    std::size_t offset,
    std::string_view value) noexcept {
    std::fill_n(
        output.data() + offset,
        rt::replay_identifier_capacity,
        std::byte{0});
    std::memcpy(output.data() + offset, value.data(), value.size());
}

void write_identifier(
    std::span<std::byte> output,
    std::size_t offset,
    const std::array<
        char,
        rt::replay_identifier_capacity>& value) noexcept {
    std::memcpy(
        output.data() + offset,
        value.data(),
        value.size());
}

std::string_view identifier_view(
    const std::array<
        char,
        rt::replay_identifier_capacity>& value) noexcept {
    const auto end = std::find(value.begin(), value.end(), '\0');
    return std::string_view(
        value.data(),
        static_cast<std::size_t>(end - value.begin()));
}

std::uint64_t checksum_excluding(
    std::span<const std::byte> bytes,
    std::size_t excluded_offset,
    std::size_t excluded_size) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value =
            index >= excluded_offset &&
                index < excluded_offset + excluded_size
            ? std::uint8_t{0}
            : static_cast<std::uint8_t>(bytes[index]);
        hash ^= value;
        hash *= kFnvPrime;
    }
    return hash;
}

bool magic_matches(
    std::span<const std::byte> bytes,
    const std::array<std::byte, 8>& magic) noexcept {
    return bytes.size() >= magic.size() &&
           std::equal(magic.begin(), magic.end(), bytes.begin());
}

bool bytes_zero(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::size_t count) noexcept {
    return offset <= bytes.size() &&
           count <= bytes.size() - offset &&
           std::all_of(
               bytes.begin() + static_cast<std::ptrdiff_t>(offset),
               bytes.begin() +
                   static_cast<std::ptrdiff_t>(offset + count),
               [](std::byte value) {
                   return value == std::byte{0};
               });
}

bool valid_tier(std::uint32_t tier) noexcept {
    return tier <= static_cast<std::uint32_t>(
        rt::DeterminismTier::portable_deterministic);
}

} // namespace

namespace rt::detail {

bool checked_artifact_add(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        result = 0;
        return false;
    }
    result = left + right;
    return true;
}

bool checked_artifact_multiply(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::size_t>::max() / left) {
        result = 0;
        return false;
    }
    result = left * right;
    return true;
}

std::uint64_t artifact_checksum(
    std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const auto value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= kFnvPrime;
    }
    return hash;
}

Status encode_checkpoint_artifact(
    CheckpointMetadata metadata,
    std::size_t state_count,
    StateWriteProvider provider,
    void* provider_context,
    std::size_t max_bytes,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept {
    result = {};
    if (!provider ||
        state_count > artifact_absolute_max_records ||
        state_count > std::numeric_limits<std::uint32_t>::max() ||
        max_bytes > artifact_absolute_max_bytes ||
        max_bytes < checkpoint_header_size ||
        !valid_tier(static_cast<std::uint32_t>(
            metadata.determinism_tier)) ||
        !valid_identifier(identifier_view(metadata.build_id)) ||
        !valid_identifier(identifier_view(metadata.workload_id))) {
        return Status::invalid_argument;
    }

    std::size_t payload_bytes = 0;
    for (std::size_t index = 0; index < state_count; ++index) {
        StateWriteView state;
        if (!provider(provider_context, index, state) ||
            !valid_identifier(state.name) ||
            state.schema_version == 0 ||
            state.payload.empty() ||
            !checked_artifact_add(
                payload_bytes,
                state.payload.size(),
                payload_bytes)) {
            return Status::invalid_argument;
        }
    }

    std::size_t record_bytes = 0;
    std::size_t required = 0;
    if (!checked_artifact_multiply(
            state_count,
            checkpoint_record_header_size,
            record_bytes) ||
        !checked_artifact_add(
            checkpoint_header_size,
            record_bytes,
            required) ||
        !checked_artifact_add(
            required,
            payload_bytes,
            required) ||
        required > max_bytes) {
        result.required_bytes = required;
        return Status::capacity_exceeded;
    }
    result.required_bytes = required;
    if (output.size() < required) {
        return Status::capacity_exceeded;
    }

    auto artifact = output.first(required);
    std::fill(artifact.begin(), artifact.end(), std::byte{0});
    std::copy(
        kCheckpointMagic.begin(),
        kCheckpointMagic.end(),
        artifact.begin());
    store_u32_le(artifact, 8, checkpoint_schema_version);
    store_u32_le(
        artifact,
        12,
        static_cast<std::uint32_t>(checkpoint_header_size));
    store_u32_le(artifact, 16, metadata.runtime_version_major);
    store_u32_le(artifact, 20, metadata.runtime_version_minor);
    store_u32_le(artifact, 24, metadata.runtime_version_patch);
    store_u32_le(
        artifact,
        28,
        static_cast<std::uint32_t>(metadata.determinism_tier));
    store_u32_le(artifact, 36, static_cast<std::uint32_t>(state_count));
    store_u32_le(
        artifact,
        40,
        static_cast<std::uint32_t>(checkpoint_record_header_size));
    store_u64_le(artifact, 48, metadata.config_id);
    store_u64_le(artifact, 56, metadata.replay_id);
    store_u64_le(artifact, 64, metadata.graph_id);
    store_u64_le(artifact, 72, metadata.state_schema_id);
    store_u64_le(
        artifact,
        80,
        metadata.checkpoint_frame_index);
    store_u64_le(
        artifact,
        88,
        static_cast<std::uint64_t>(payload_bytes));
    store_u64_le(
        artifact,
        96,
        static_cast<std::uint64_t>(required));
    write_identifier(artifact, 120, metadata.build_id);
    write_identifier(artifact, 184, metadata.workload_id);

    std::size_t offset = checkpoint_header_size;
    for (std::size_t index = 0; index < state_count; ++index) {
        StateWriteView state;
        if (!provider(provider_context, index, state)) {
            return Status::internal_error;
        }
        write_identifier(artifact, offset, state.name);
        store_u32_le(artifact, offset + 64, state.schema_version);
        store_u64_le(
            artifact,
            offset + 72,
            static_cast<std::uint64_t>(state.payload.size()));
        store_u64_le(
            artifact,
            offset + 80,
            artifact_checksum(state.payload));
        offset += checkpoint_record_header_size;
        std::memcpy(
            artifact.data() + offset,
            state.payload.data(),
            state.payload.size());
        offset += state.payload.size();
    }

    const auto state_hash = artifact_checksum(
        artifact.subspan(checkpoint_header_size));
    store_u64_le(artifact, 104, state_hash);
    const auto checksum = checksum_excluding(
        artifact,
        kCheckpointChecksumOffset,
        sizeof(std::uint64_t));
    store_u64_le(artifact, kCheckpointChecksumOffset, checksum);
    result.bytes_written = required;
    result.checksum = checksum;
    return Status::ok;
}

Status parse_checkpoint_artifact(
    std::span<const std::byte> artifact,
    std::size_t max_bytes,
    std::size_t max_states,
    CheckpointMetadata& metadata) noexcept {
    metadata = {};
    if (max_bytes > artifact_absolute_max_bytes ||
        max_states > artifact_absolute_max_records ||
        artifact.size() < checkpoint_header_size ||
        artifact.size() > max_bytes ||
        !magic_matches(artifact, kCheckpointMagic)) {
        return Status::invalid_artifact;
    }

    std::uint32_t schema = 0;
    std::uint32_t header_size = 0;
    std::uint32_t tier = 0;
    std::uint32_t flags = 0;
    std::uint32_t state_count = 0;
    std::uint32_t record_size = 0;
    std::uint32_t reserved = 0;
    std::uint64_t total_bytes = 0;
    if (!load_u32_le(artifact, 8, schema) ||
        !load_u32_le(artifact, 12, header_size) ||
        !load_u32_le(artifact, 16, metadata.runtime_version_major) ||
        !load_u32_le(artifact, 20, metadata.runtime_version_minor) ||
        !load_u32_le(artifact, 24, metadata.runtime_version_patch) ||
        !load_u32_le(artifact, 28, tier) ||
        !load_u32_le(artifact, 32, flags) ||
        !load_u32_le(artifact, 36, state_count) ||
        !load_u32_le(artifact, 40, record_size) ||
        !load_u32_le(artifact, 44, reserved) ||
        !load_u64_le(artifact, 48, metadata.config_id) ||
        !load_u64_le(artifact, 56, metadata.replay_id) ||
        !load_u64_le(artifact, 64, metadata.graph_id) ||
        !load_u64_le(artifact, 72, metadata.state_schema_id) ||
        !load_u64_le(
            artifact,
            80,
            metadata.checkpoint_frame_index) ||
        !load_u64_le(
            artifact,
            88,
            metadata.state_payload_bytes) ||
        !load_u64_le(artifact, 96, total_bytes) ||
        !load_u64_le(artifact, 104, metadata.state_hash) ||
        !load_u64_le(
            artifact,
            kCheckpointChecksumOffset,
            metadata.artifact_checksum) ||
        !fixed_identifier(artifact, 120, metadata.build_id) ||
        !fixed_identifier(artifact, 184, metadata.workload_id) ||
        schema != checkpoint_schema_version ||
        header_size != checkpoint_header_size ||
        record_size != checkpoint_record_header_size ||
        flags != 0 ||
        reserved != 0 ||
        !bytes_zero(artifact, 248, 8) ||
        !valid_tier(tier) ||
        state_count > max_states ||
        total_bytes != artifact.size() ||
        metadata.state_payload_bytes > max_bytes) {
        metadata = {};
        return Status::invalid_artifact;
    }
    metadata.schema_version = schema;
    metadata.determinism_tier =
        static_cast<DeterminismTier>(tier);
    metadata.state_count = state_count;
    metadata.total_bytes = total_bytes;

    if (checksum_excluding(
            artifact,
            kCheckpointChecksumOffset,
            sizeof(std::uint64_t)) !=
        metadata.artifact_checksum) {
        metadata = {};
        return Status::invalid_artifact;
    }

    std::size_t offset = checkpoint_header_size;
    std::size_t payload_total = 0;
    for (std::uint32_t index = 0;
         index < state_count;
         ++index) {
        if (offset > artifact.size() ||
            artifact.size() - offset <
                checkpoint_record_header_size) {
            metadata = {};
            return Status::invalid_artifact;
        }
        std::string_view name;
        std::uint32_t state_schema = 0;
        std::uint32_t state_reserved = 0;
        std::uint64_t payload_size = 0;
        std::uint64_t payload_checksum = 0;
        if (!fixed_name(artifact, offset, name) ||
            !load_u32_le(
                artifact,
                offset + 64,
                state_schema) ||
            !load_u32_le(
                artifact,
                offset + 68,
                state_reserved) ||
            !load_u64_le(
                artifact,
                offset + 72,
                payload_size) ||
            !load_u64_le(
                artifact,
                offset + 80,
                payload_checksum) ||
            state_schema == 0 ||
            state_reserved != 0 ||
            payload_size == 0 ||
            payload_size >
                std::numeric_limits<std::size_t>::max()) {
            metadata = {};
            return Status::invalid_artifact;
        }
        offset += checkpoint_record_header_size;
        const auto size = static_cast<std::size_t>(payload_size);
        if (size > artifact.size() - offset ||
            !checked_artifact_add(
                payload_total,
                size,
                payload_total)) {
            metadata = {};
            return Status::invalid_artifact;
        }
        const auto payload = artifact.subspan(offset, size);
        if (artifact_checksum(payload) != payload_checksum) {
            metadata = {};
            return Status::invalid_artifact;
        }
        offset += size;
    }

    if (offset != artifact.size() ||
        payload_total != metadata.state_payload_bytes ||
        artifact_checksum(
            artifact.subspan(checkpoint_header_size)) !=
            metadata.state_hash) {
        metadata = {};
        return Status::invalid_artifact;
    }
    return Status::ok;
}

bool next_checkpoint_record(
    std::span<const std::byte> artifact,
    const CheckpointMetadata& metadata,
    CheckpointRecordCursor& cursor,
    CheckpointRecordView& record) noexcept {
    record = {};
    if (cursor.index >= metadata.state_count ||
        cursor.offset > artifact.size() ||
        artifact.size() - cursor.offset <
            checkpoint_record_header_size) {
        return false;
    }
    std::uint32_t reserved = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t payload_checksum = 0;
    if (!fixed_name(artifact, cursor.offset, record.name) ||
        !load_u32_le(
            artifact,
            cursor.offset + 64,
            record.schema_version) ||
        !load_u32_le(
            artifact,
            cursor.offset + 68,
            reserved) ||
        !load_u64_le(
            artifact,
            cursor.offset + 72,
            payload_size) ||
        !load_u64_le(
            artifact,
            cursor.offset + 80,
            payload_checksum) ||
        reserved != 0 ||
        payload_size >
            std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    cursor.offset += checkpoint_record_header_size;
    const auto size = static_cast<std::size_t>(payload_size);
    if (size > artifact.size() - cursor.offset) {
        return false;
    }
    record.payload = artifact.subspan(cursor.offset, size);
    cursor.offset += size;
    ++cursor.index;
    return artifact_checksum(record.payload) == payload_checksum;
}

Status encode_input_log_artifact(
    InputLogMetadata metadata,
    std::span<const ReplayInputRecord> records,
    std::size_t max_records,
    std::size_t max_bytes,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept {
    result = {};
    if (records.size() > max_records ||
        records.size() > artifact_absolute_max_records ||
        records.size() > std::numeric_limits<std::uint32_t>::max() ||
        max_records > artifact_absolute_max_records ||
        max_bytes > artifact_absolute_max_bytes ||
        max_bytes < input_log_header_size ||
        !valid_tier(static_cast<std::uint32_t>(
            metadata.determinism_tier)) ||
        !valid_identifier(identifier_view(metadata.workload_id))) {
        return Status::invalid_argument;
    }

    std::size_t payload_bytes = 0;
    std::uint64_t previous_frame = 0;
    bool have_previous = false;
    for (const auto& record : records) {
        if (record.frame.delta.count() < 0 ||
            record.payload.size() >
                std::numeric_limits<std::uint32_t>::max() ||
            (have_previous &&
             record.frame.frame_index <= previous_frame) ||
            !checked_artifact_add(
                payload_bytes,
                record.payload.size(),
                payload_bytes)) {
            return Status::invalid_argument;
        }
        previous_frame = record.frame.frame_index;
        have_previous = true;
    }

    std::size_t record_bytes = 0;
    std::size_t required = 0;
    if (!checked_artifact_multiply(
            records.size(),
            input_log_record_header_size,
            record_bytes) ||
        !checked_artifact_add(
            input_log_header_size,
            record_bytes,
            required) ||
        !checked_artifact_add(
            required,
            payload_bytes,
            required) ||
        required > max_bytes) {
        result.required_bytes = required;
        return Status::capacity_exceeded;
    }
    result.required_bytes = required;
    if (output.size() < required) {
        return Status::capacity_exceeded;
    }

    auto artifact = output.first(required);
    std::fill(artifact.begin(), artifact.end(), std::byte{0});
    std::copy(
        kInputLogMagic.begin(),
        kInputLogMagic.end(),
        artifact.begin());
    store_u32_le(artifact, 8, input_log_schema_version);
    store_u32_le(
        artifact,
        12,
        static_cast<std::uint32_t>(input_log_header_size));
    store_u32_le(artifact, 16, metadata.runtime_version_major);
    store_u32_le(artifact, 20, metadata.runtime_version_minor);
    store_u32_le(artifact, 24, metadata.runtime_version_patch);
    store_u32_le(
        artifact,
        28,
        static_cast<std::uint32_t>(metadata.determinism_tier));
    store_u32_le(
        artifact,
        36,
        static_cast<std::uint32_t>(records.size()));
    store_u32_le(
        artifact,
        40,
        static_cast<std::uint32_t>(input_log_record_header_size));
    store_u64_le(artifact, 48, metadata.replay_id);
    store_u64_le(artifact, 56, metadata.state_schema_id);
    store_u64_le(
        artifact,
        64,
        static_cast<std::uint64_t>(payload_bytes));
    store_u64_le(
        artifact,
        72,
        static_cast<std::uint64_t>(required));
    if (!records.empty()) {
        store_u64_le(
            artifact,
            88,
            records.front().frame.frame_index);
        store_u64_le(
            artifact,
            96,
            records.back().frame.frame_index);
    }
    write_identifier(artifact, 104, metadata.workload_id);

    std::size_t offset = input_log_header_size;
    for (const auto& record : records) {
        store_u64_le(
            artifact,
            offset,
            record.frame.frame_index);
        store_u64_le(
            artifact,
            offset + 8,
            static_cast<std::uint64_t>(
                record.frame.delta.count()));
        store_u64_le(
            artifact,
            offset + 16,
            record.frame.deadline_ns.value_or(0));
        store_u32_le(artifact, offset + 24, record.input_type);
        store_u32_le(
            artifact,
            offset + 28,
            static_cast<std::uint32_t>(record.payload.size()));
        store_u32_le(
            artifact,
            offset + 32,
            record.frame.deadline_ns ? 1u : 0u);
        store_u64_le(
            artifact,
            offset + 40,
            artifact_checksum(record.payload));
        offset += input_log_record_header_size;
        if (!record.payload.empty()) {
            std::memcpy(
                artifact.data() + offset,
                record.payload.data(),
                record.payload.size());
            offset += record.payload.size();
        }
    }

    const auto checksum = checksum_excluding(
        artifact,
        kInputLogChecksumOffset,
        sizeof(std::uint64_t));
    store_u64_le(artifact, kInputLogChecksumOffset, checksum);
    result.bytes_written = required;
    result.checksum = checksum;
    return Status::ok;
}

Status parse_input_log_artifact(
    std::span<const std::byte> artifact,
    std::size_t max_bytes,
    std::size_t max_records,
    InputLogMetadata& metadata) noexcept {
    metadata = {};
    if (max_bytes > artifact_absolute_max_bytes ||
        max_records > artifact_absolute_max_records ||
        artifact.size() < input_log_header_size ||
        artifact.size() > max_bytes ||
        !magic_matches(artifact, kInputLogMagic)) {
        return Status::invalid_artifact;
    }

    std::uint32_t schema = 0;
    std::uint32_t header_size = 0;
    std::uint32_t tier = 0;
    std::uint32_t flags = 0;
    std::uint32_t record_count = 0;
    std::uint32_t record_size = 0;
    std::uint32_t reserved = 0;
    std::uint64_t total_bytes = 0;
    if (!load_u32_le(artifact, 8, schema) ||
        !load_u32_le(artifact, 12, header_size) ||
        !load_u32_le(artifact, 16, metadata.runtime_version_major) ||
        !load_u32_le(artifact, 20, metadata.runtime_version_minor) ||
        !load_u32_le(artifact, 24, metadata.runtime_version_patch) ||
        !load_u32_le(artifact, 28, tier) ||
        !load_u32_le(artifact, 32, flags) ||
        !load_u32_le(artifact, 36, record_count) ||
        !load_u32_le(artifact, 40, record_size) ||
        !load_u32_le(artifact, 44, reserved) ||
        !load_u64_le(artifact, 48, metadata.replay_id) ||
        !load_u64_le(artifact, 56, metadata.state_schema_id) ||
        !load_u64_le(artifact, 64, metadata.payload_bytes) ||
        !load_u64_le(artifact, 72, total_bytes) ||
        !load_u64_le(
            artifact,
            kInputLogChecksumOffset,
            metadata.artifact_checksum) ||
        !load_u64_le(
            artifact,
            88,
            metadata.first_frame_index) ||
        !load_u64_le(
            artifact,
            96,
            metadata.last_frame_index) ||
        !fixed_identifier(artifact, 104, metadata.workload_id) ||
        schema != input_log_schema_version ||
        header_size != input_log_header_size ||
        record_size != input_log_record_header_size ||
        flags != 0 ||
        reserved != 0 ||
        !bytes_zero(artifact, 168, 24) ||
        !valid_tier(tier) ||
        record_count > max_records ||
        total_bytes != artifact.size() ||
        metadata.payload_bytes > max_bytes) {
        metadata = {};
        return Status::invalid_artifact;
    }
    metadata.schema_version = schema;
    metadata.determinism_tier =
        static_cast<DeterminismTier>(tier);
    metadata.record_count = record_count;
    metadata.total_bytes = total_bytes;

    if (checksum_excluding(
            artifact,
            kInputLogChecksumOffset,
            sizeof(std::uint64_t)) !=
        metadata.artifact_checksum) {
        metadata = {};
        return Status::invalid_artifact;
    }

    std::size_t offset = input_log_header_size;
    std::size_t payload_total = 0;
    std::uint64_t previous_frame = 0;
    for (std::uint32_t index = 0;
         index < record_count;
         ++index) {
        if (offset > artifact.size() ||
            artifact.size() - offset <
                input_log_record_header_size) {
            metadata = {};
            return Status::invalid_artifact;
        }
        std::uint64_t frame_index = 0;
        std::uint64_t delta_ns = 0;
        std::uint64_t deadline_ns = 0;
        std::uint32_t input_type = 0;
        std::uint32_t payload_size = 0;
        std::uint32_t record_flags = 0;
        std::uint32_t record_reserved = 0;
        std::uint64_t payload_checksum = 0;
        if (!load_u64_le(artifact, offset, frame_index) ||
            !load_u64_le(artifact, offset + 8, delta_ns) ||
            !load_u64_le(artifact, offset + 16, deadline_ns) ||
            !load_u32_le(artifact, offset + 24, input_type) ||
            !load_u32_le(artifact, offset + 28, payload_size) ||
            !load_u32_le(artifact, offset + 32, record_flags) ||
            !load_u32_le(
                artifact,
                offset + 36,
                record_reserved) ||
            !load_u64_le(
                artifact,
                offset + 40,
                payload_checksum) ||
            delta_ns >
                static_cast<std::uint64_t>(
                    std::chrono::nanoseconds::max().count()) ||
            (record_flags & ~1u) != 0 ||
            record_reserved != 0 ||
            ((record_flags & 1u) == 0 && deadline_ns != 0) ||
            (index != 0 && frame_index <= previous_frame)) {
            (void)input_type;
            metadata = {};
            return Status::invalid_artifact;
        }
        offset += input_log_record_header_size;
        if (payload_size > artifact.size() - offset ||
            !checked_artifact_add(
                payload_total,
                payload_size,
                payload_total)) {
            metadata = {};
            return Status::invalid_artifact;
        }
        const auto payload = artifact.subspan(
            offset,
            payload_size);
        if (artifact_checksum(payload) != payload_checksum) {
            metadata = {};
            return Status::invalid_artifact;
        }
        offset += payload_size;
        previous_frame = frame_index;
    }

    if (offset != artifact.size() ||
        payload_total != metadata.payload_bytes ||
        (record_count == 0 &&
         (metadata.first_frame_index != 0 ||
          metadata.last_frame_index != 0)) ||
        (record_count != 0 &&
         (metadata.first_frame_index >
              metadata.last_frame_index ||
          previous_frame != metadata.last_frame_index))) {
        metadata = {};
        return Status::invalid_artifact;
    }
    if (record_count != 0) {
        std::uint64_t first = 0;
        if (!load_u64_le(artifact, input_log_header_size, first) ||
            first != metadata.first_frame_index) {
            metadata = {};
            return Status::invalid_artifact;
        }
    }
    return Status::ok;
}

bool next_input_log_record(
    std::span<const std::byte> artifact,
    const InputLogMetadata& metadata,
    InputLogRecordCursor& cursor,
    InputLogRecordView& record) noexcept {
    record = {};
    if (cursor.index >= metadata.record_count ||
        cursor.offset > artifact.size() ||
        artifact.size() - cursor.offset <
            input_log_record_header_size) {
        return false;
    }
    std::uint64_t delta_ns = 0;
    std::uint64_t deadline_ns = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
    std::uint64_t payload_checksum = 0;
    if (!load_u64_le(
            artifact,
            cursor.offset,
            record.frame.frame_index) ||
        !load_u64_le(
            artifact,
            cursor.offset + 8,
            delta_ns) ||
        !load_u64_le(
            artifact,
            cursor.offset + 16,
            deadline_ns) ||
        !load_u32_le(
            artifact,
            cursor.offset + 24,
            record.input_type) ||
        !load_u32_le(
            artifact,
            cursor.offset + 28,
            payload_size) ||
        !load_u32_le(
            artifact,
            cursor.offset + 32,
            flags) ||
        !load_u32_le(
            artifact,
            cursor.offset + 36,
            reserved) ||
        !load_u64_le(
            artifact,
            cursor.offset + 40,
            payload_checksum) ||
        flags > 1u ||
        reserved != 0 ||
        delta_ns >
            static_cast<std::uint64_t>(
                std::chrono::nanoseconds::max().count())) {
        return false;
    }
    record.frame.delta = std::chrono::nanoseconds(
        static_cast<std::chrono::nanoseconds::rep>(delta_ns));
    if ((flags & 1u) != 0) {
        record.frame.deadline_ns = deadline_ns;
    }
    cursor.offset += input_log_record_header_size;
    if (payload_size > artifact.size() - cursor.offset) {
        return false;
    }
    record.payload = artifact.subspan(
        cursor.offset,
        payload_size);
    cursor.offset += payload_size;
    ++cursor.index;
    return artifact_checksum(record.payload) == payload_checksum;
}

} // namespace rt::detail

namespace rt {

Status inspect_checkpoint_artifact(
    std::span<const std::byte> artifact,
    CheckpointMetadata& metadata) noexcept {
    return detail::parse_checkpoint_artifact(
        artifact,
        detail::artifact_absolute_max_bytes,
        detail::artifact_absolute_max_records,
        metadata);
}

Status inspect_input_log_artifact(
    std::span<const std::byte> artifact,
    InputLogMetadata& metadata) noexcept {
    return detail::parse_input_log_artifact(
        artifact,
        detail::artifact_absolute_max_bytes,
        detail::artifact_absolute_max_records,
        metadata);
}

} // namespace rt

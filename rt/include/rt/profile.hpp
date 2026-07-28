#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "rt/config.hpp"
// Retain the 1.x transitive umbrella contract. New code that only needs
// configuration types should include <rt/config.hpp> directly.
#include "rt/runtime.hpp"

namespace rt {

inline constexpr std::uint32_t runtime_profile_schema_version = 1;
inline constexpr std::size_t runtime_profile_identifier_capacity = 64;
inline constexpr std::size_t runtime_profile_error_path_capacity = 96;
inline constexpr std::size_t runtime_profile_max_bytes = 64 * 1024;

enum class RuntimeProfileErrorCode : std::uint8_t {
    none = 0,
    input_too_large,
    syntax,
    duplicate_key,
    unknown_key,
    missing_key,
    invalid_type,
    invalid_value,
    incompatible_schema,
    incompatible_runtime,
};

struct RuntimeProfileMetadata {
    std::uint32_t schema_version = 0;
    std::uint32_t runtime_config_schema = 0;
    std::uint32_t runtime_major = 0;
    std::uint32_t minimum_runtime_minor = 0;
    std::array<char, runtime_profile_identifier_capacity> profile_id{};
};

struct RuntimeProfileError {
    RuntimeProfileErrorCode code = RuntimeProfileErrorCode::none;
    std::size_t byte_offset = 0;
    std::array<char, runtime_profile_error_path_capacity> path{};
};

// Parses one complete, resolved runtime profile from caller-owned UTF-8 JSON.
// Parsing is bounded and allocation-free. Unknown or duplicate contract keys,
// omitted runtime fields, incompatible schemas, invalid values, malformed
// UTF-8/JSON, and trailing input fail closed. The optional params object is
// validated as bounded JSON provenance but is not interpreted by the runtime.
// On failure config and metadata are unchanged.
[[nodiscard]] Status parse_runtime_profile(
    std::string_view json,
    RuntimeConfig& config,
    RuntimeProfileMetadata& metadata,
    RuntimeProfileError& error) noexcept;

[[nodiscard]] const char* runtime_profile_error_message(
    RuntimeProfileErrorCode code) noexcept;

} // namespace rt

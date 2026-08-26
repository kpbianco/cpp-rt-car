#include <rt/profile.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace {

rt::RuntimeConfig sentinel_config() {
    rt::RuntimeConfig config;
    config.callback_capacity = 7;
    config.worker_count = 3;
    config.memory_budget_bytes = 123456;
    constexpr char workload[] = "fuzz-sentinel";
    std::memcpy(config.workload_id.data(), workload, sizeof(workload));
    return config;
}

rt::RuntimeProfileMetadata sentinel_metadata() {
    rt::RuntimeProfileMetadata metadata;
    metadata.schema_version = 99;
    metadata.runtime_config_schema = 98;
    metadata.runtime_major = 97;
    metadata.minimum_runtime_minor = 96;
    constexpr char value[] = "fuzz-sentinel";
    std::memcpy(metadata.profile_id.data(), value, sizeof(value));
    return metadata;
}

bool same_config(
    const rt::RuntimeConfig& left,
    const rt::RuntimeConfig& right) {
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

bool same_metadata(
    const rt::RuntimeProfileMetadata& left,
    const rt::RuntimeProfileMetadata& right) {
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size) {
    if (size > rt::runtime_profile_max_bytes) {
        return 0;
    }

    const auto before_config = sentinel_config();
    const auto before_metadata = sentinel_metadata();
    auto config = before_config;
    auto metadata = before_metadata;
    rt::RuntimeProfileError error;
    const auto status = rt::parse_runtime_profile(
        std::string_view(
            reinterpret_cast<const char*>(data),
            size),
        config,
        metadata,
        error);
    if (status != rt::Status::ok &&
        (!same_config(config, before_config) ||
         !same_metadata(metadata, before_metadata))) {
        __builtin_trap();
    }
    return 0;
}

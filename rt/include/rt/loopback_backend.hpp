#pragma once

#include <rt/runtime.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace rt {

inline constexpr std::size_t sampled_io_loopback_capacity = 16;

struct SampledIoLoopbackConfig {
    std::uint32_t queue_capacity = 8;
    std::uint32_t buffer_capacity = 8;
    std::uint64_t maximum_buffer_bytes = 1u << 20u;
    std::uint64_t memory_domain_identity = 1;
    std::uint64_t timestamp_domain_identity = 1;
};

struct SampledIoLoopbackRoute {
    std::uint32_t opcode = 0;
    std::uint64_t source_channel_identity = 0;
    std::uint64_t destination_channel_identity = 0;
    std::uint64_t destination_timestamp_domain_identity = 0;
    std::uint64_t destination_calibration_identity = 0;
    std::uint64_t destination_trigger_identity = 0;
};

struct SampledIoLoopbackStats {
    std::uint64_t submissions = 0;
    std::uint64_t completions = 0;
    std::uint64_t cancellations = 0;
    std::uint64_t rejected = 0;
    std::uint64_t frames_copied = 0;
};

enum class SampledIoLoopbackFault : std::uint8_t {
    none = 0,
    reject_submission = 1,
    malformed_sequence = 2,
    completion_error = 3,
    completion_timeout = 4,
    completion_lost = 5,
};

// A bounded, deterministic public HAL-v2 backend for sampled-I/O integration,
// examples, and portable tests. Each configured dispatch opcode copies buffer
// reference 0 to reference 1 and rewrites the sampled-frame destination
// identity fields. It performs no allocation after construction.
class SampledIoLoopbackBackend final {
public:
    explicit SampledIoLoopbackBackend(
        const SampledIoLoopbackConfig& config = {});
    ~SampledIoLoopbackBackend();

    SampledIoLoopbackBackend(const SampledIoLoopbackBackend&) = delete;
    SampledIoLoopbackBackend& operator=(
        const SampledIoLoopbackBackend&) = delete;
    SampledIoLoopbackBackend(SampledIoLoopbackBackend&&) noexcept;
    SampledIoLoopbackBackend& operator=(
        SampledIoLoopbackBackend&&) noexcept;

    [[nodiscard]] Status add_route(
        const SampledIoLoopbackRoute& route) noexcept;
    // One bounded atomic injection consumed by the next batch submission.
    [[nodiscard]] Status inject_next(
        SampledIoLoopbackFault fault) noexcept;
    [[nodiscard]] HalV2BackendRegistration hal_v2_registration(
        std::string_view name = "sampled_io.loopback") noexcept;
    [[nodiscard]] SampledIoLoopbackStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rt

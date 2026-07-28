#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <rt/status.hpp>

namespace rt {

inline constexpr std::uint32_t runtime_config_schema_version = 7;
inline constexpr std::size_t observability_identifier_capacity = 64;

enum class NumericalMode : std::uint8_t {
    precise,
    fused_multiply_add,
};

enum class ExecutorPolicy : std::uint8_t {
    static_deterministic,
    bounded_throughput,
    host_adapter,
};

enum class OverloadPolicy : std::uint8_t {
    reject_submission,
    fail_frame,
};

enum class PlatformPreflightMode : std::uint8_t {
    disabled,
    strict,
};

enum class DeterminismTier : std::uint8_t {
    unspecified = 0,
    schedule_independent = 1,
    reproducible_build = 2,
    portable_deterministic = 3,
};

struct RuntimeConfig {
    std::size_t callback_capacity = 64;
    std::size_t scratch_bytes = 64 * 1024;
    std::size_t trace_capacity = 1024;
    NumericalMode numerical_mode = NumericalMode::precise;
    ExecutorPolicy executor_policy = ExecutorPolicy::static_deterministic;
    std::size_t worker_count = 1;
    std::size_t executor_queue_capacity = 1024;
    std::size_t scratch_alignment = 64;
    std::size_t task_scratch_bytes = 4 * 1024;
    std::size_t task_scratch_slots = 1024;
    std::size_t memory_budget_bytes = 256 * 1024 * 1024;
    OverloadPolicy overload_policy = OverloadPolicy::reject_submission;
    std::uint64_t watchdog_timeout_ns = 0;
    std::uint32_t watchdog_max_degradation_level = 0;
    PlatformPreflightMode platform_preflight_mode =
        PlatformPreflightMode::disabled;
    // D1 is an explicit application contract: only registered canonical state
    // is compared, and callbacks must obey the restrictions documented in
    // docs/determinism_replay.md. D2 and D3 are reserved and rejected.
    DeterminismTier determinism_tier = DeterminismTier::unspecified;
    std::size_t state_capacity = 64;
    std::size_t snapshot_max_bytes = 1024 * 1024;
    std::size_t replay_input_capacity = 4096;
    std::size_t input_log_max_bytes = 1024 * 1024;
    std::size_t device_backend_capacity = 1;
    std::size_t device_buffer_capacity = 64;
    std::size_t device_outstanding_capacity = 64;
    std::size_t device_completion_batch = 16;
    // Stable caller-supplied provenance copied into every observability
    // snapshot. IDs use [A-Za-z0-9._:/@-] and must be NUL terminated.
    std::array<char, observability_identifier_capacity> workload_id{
        'u', 'n', 's', 'p', 'e', 'c', 'i', 'f', 'i', 'e', 'd', '\0'};
};

// Applies one strict schema key to a typed configuration. The supported keys
// are callback_capacity, scratch_bytes, trace_capacity, numerical_mode,
// executor_policy, worker_count, executor_queue_capacity, scratch_alignment,
// task_scratch_bytes, task_scratch_slots, memory_budget_bytes,
// overload_policy, watchdog_timeout_ns, watchdog_max_degradation_level,
// platform_preflight_mode, determinism_tier, state_capacity,
// snapshot_max_bytes, replay_input_capacity, input_log_max_bytes,
// device_backend_capacity, device_buffer_capacity,
// device_outstanding_capacity, device_completion_batch, and workload_id.
// Unknown keys and partially parsed values are rejected.
[[nodiscard]] Status set_runtime_config_value(
    RuntimeConfig& config,
    std::string_view key,
    std::string_view value) noexcept;

} // namespace rt

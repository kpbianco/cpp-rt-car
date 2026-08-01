#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <rt/status.hpp>

namespace rt {

inline constexpr std::uint32_t runtime_config_schema_version = 7;
inline constexpr std::size_t observability_identifier_capacity = 64;
inline constexpr std::size_t policy_thread_name_capacity = 64;
inline constexpr std::size_t policy_cpu_capacity = 256;
inline constexpr std::size_t policy_cpu_word_count =
    policy_cpu_capacity / 64;
inline constexpr std::size_t policy_request_capacity = 1024;

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

// M15 policy types are a C++ source contract. They are deliberately separate
// from RuntimeConfig schema 7 and stable C ABI v8.
enum class PolicyRequirement : std::uint8_t {
    best_effort,
    required,
};

enum class ThreadRole : std::uint16_t {
    none = 0,
    frame = 1,
    executor_worker = 2,
    watchdog_service = 3,
    device_service = 4,
    xdma_io = 5,
    accelerator_submission = 6,
};

struct ThreadResourceId {
    ThreadRole role = ThreadRole::none;
    std::uint32_t instance = 0;

    friend constexpr bool operator==(
        ThreadResourceId,
        ThreadResourceId) noexcept = default;
};

struct CpuSetRequest {
    // When specified is false, logical_cpu_count and words must be zero.
    bool specified = false;
    std::uint16_t logical_cpu_count = 0;
    std::array<std::uint64_t, policy_cpu_word_count> words{};
};

enum class SchedulingClass : std::uint8_t {
    inherit,
    normal,
    fifo,
    round_robin,
};

enum class WaitStrategy : std::uint8_t {
    runtime_default,
    spin,
    yield,
    park,
    adaptive,
};

struct ThreadPolicy {
    PolicyRequirement requirement = PolicyRequirement::best_effort;
    CpuSetRequest cpu_set{};
    SchedulingClass scheduling_class = SchedulingClass::inherit;
    std::uint8_t scheduling_priority = 0;
    // -1 inherits the current placement; nonnegative values request a node.
    std::int32_t numa_node = -1;
    WaitStrategy wait_strategy = WaitStrategy::runtime_default;
    // Zero preserves the implementation/host default.
    std::size_t stack_bytes = 0;
    std::size_t guard_bytes = 0;
    // Empty means no name request. Nonempty names must be NUL terminated and
    // use the bounded portable identifier character set.
    std::array<char, policy_thread_name_capacity> name{};
};

struct ThreadPolicyRequest {
    ThreadResourceId id{};
    ThreadPolicy policy{};
};

enum class MemoryCategory : std::uint16_t {
    runtime_control = 1,
    executor_control_and_queues = 2,
    device_control_and_queues = 3,
    phase_scratch = 4,
    task_scratch = 5,
    trace_storage = 6,
    thread_stack = 7,
    backend_storage = 8,
    registered_state = 9,
    registered_device_buffer = 10,
};

struct MemoryRegionId {
    MemoryCategory category = MemoryCategory::runtime_control;
    // Only thread_stack uses thread_role; all other categories require none.
    ThreadRole thread_role = ThreadRole::none;
    std::uint32_t instance = 0;

    friend constexpr bool operator==(
        MemoryRegionId,
        MemoryRegionId) noexcept = default;
};

enum class MemoryProviderOwnership : std::uint8_t {
    inherit,
    runtime,
    host,
    backend,
};

enum class MemoryPolicyToggle : std::uint8_t {
    runtime_default,
    disabled,
    enabled,
};

enum class HugePagePolicy : std::uint8_t {
    runtime_default,
    disabled,
    prefer,
    require,
};

enum class FirstTouchPolicy : std::uint8_t {
    runtime_default,
    disabled,
    frame_thread,
    owner_thread,
};

enum class RollbackIntent : std::uint8_t {
    runtime_default,
    release,
    return_to_provider,
    retain_external,
};

struct MemoryRegionPolicy {
    PolicyRequirement requirement = PolicyRequirement::best_effort;
    MemoryProviderOwnership provider = MemoryProviderOwnership::inherit;
    // Zero preserves the current region alignment.
    std::size_t alignment = 0;
    MemoryPolicyToggle page_rounding = MemoryPolicyToggle::runtime_default;
    std::size_t guard_before_bytes = 0;
    std::size_t guard_after_bytes = 0;
    MemoryPolicyToggle prefault = MemoryPolicyToggle::runtime_default;
    MemoryPolicyToggle locking = MemoryPolicyToggle::runtime_default;
    MemoryPolicyToggle pinning = MemoryPolicyToggle::runtime_default;
    HugePagePolicy huge_pages = HugePagePolicy::runtime_default;
    bool allow_huge_page_fallback = false;
    std::int32_t numa_node = -1;
    FirstTouchPolicy first_touch = FirstTouchPolicy::runtime_default;
    MemoryPolicyToggle residency_verification =
        MemoryPolicyToggle::runtime_default;
    RollbackIntent rollback = RollbackIntent::runtime_default;
};

struct MemoryPolicyRequest {
    MemoryRegionId id{};
    MemoryRegionPolicy policy{};
};

struct CpuMemoryPolicyRequest {
    std::span<const ThreadPolicyRequest> threads{};
    std::span<const MemoryPolicyRequest> memory_regions{};
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

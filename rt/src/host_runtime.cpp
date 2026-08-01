#include <rt/runtime.hpp>

#include "aligned_storage.hpp"
#include "compiled_graph.hpp"
#include "cpu_memory_policy.hpp"
#include "native_thread_policy.hpp"
#include "thread_policy_transaction.hpp"
#include "device_manager.hpp"
#include "executor.hpp"
#include "native_platform_preflight.hpp"
#include "snapshot_codec.hpp"
#include "telemetry.hpp"
#include "watchdog_monitor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <rt/arch.hpp>
#include <rt/canonical_bytes.hpp>

namespace {

constexpr std::size_t kMaxCallbacks = 65'536;
constexpr std::size_t kMaxResources = 65'536;
constexpr std::size_t kMaxScratchBytes = std::size_t{1} << 30;
constexpr std::size_t kMaxTraceEvents = std::size_t{1} << 20;
constexpr std::size_t kMaxWorkers = 256;
constexpr std::size_t kMaxExecutorQueueCapacity = std::size_t{1} << 20;
constexpr std::size_t kMaxTaskScratchBytes = std::size_t{1} << 20;
constexpr std::size_t kMaxTaskScratchSlots = std::size_t{1} << 20;
constexpr std::size_t kMaxScratchAlignment = std::size_t{1} << 12;
constexpr std::size_t kMaxMemoryBudgetBytes =
    sizeof(std::size_t) >= sizeof(std::uint64_t)
    ? static_cast<std::size_t>(std::uint64_t{1} << 40)
    : std::numeric_limits<std::size_t>::max();
constexpr std::uint64_t kMaxWatchdogTimeoutNs =
    std::uint64_t{24} * 60 * 60 * 1'000'000'000;
constexpr std::uint32_t kMaxDegradationLevel = 255;
constexpr std::size_t kMaxRegisteredStates =
    rt::detail::artifact_absolute_max_records;
constexpr std::size_t kMaxArtifactBytes =
    rt::detail::artifact_absolute_max_bytes;
constexpr std::size_t kMaxReplayInputs =
    rt::detail::artifact_absolute_max_records;
constexpr std::size_t kMaxDeviceBackends = 256;
constexpr std::size_t kMaxDeviceBuffers = 65'536;
constexpr std::size_t kMaxDeviceOutstanding = std::size_t{1} << 20;
constexpr std::size_t kMaxDeviceCompletionBatch = std::size_t{1} << 16;
constexpr std::size_t kNoCallback = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kNoWorker = std::numeric_limits<std::size_t>::max();
constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ull;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ull;

#ifndef RTFW_BUILD_ID_STRING
#define RTFW_BUILD_ID_STRING "rtfw-" RTFW_VERSION_STRING
#endif

constexpr char kBuildId[] = RTFW_BUILD_ID_STRING;
static_assert(
    sizeof(kBuildId) <= rt::observability_identifier_capacity,
    "RTFW_BUILD_ID_STRING exceeds the observability identifier capacity");

std::atomic<std::uint32_t> g_next_graph_owner{1};

std::uint32_t next_graph_owner() noexcept {
    for (;;) {
        const auto candidate =
            g_next_graph_owner.fetch_add(1, std::memory_order_relaxed);
        if (candidate != 0 &&
            candidate != std::numeric_limits<std::uint32_t>::max()) {
            return candidate;
        }
    }
}

bool checked_time_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        result = 0;
        return false;
    }
    result = left + right;
    return true;
}

bool checked_time_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        result = 0;
        return false;
    }
    result = left * right;
    return true;
}

bool byte_spans_overlap(
    std::span<const std::byte> left,
    std::span<const std::byte> right) noexcept {
    if (left.empty() || right.empty()) {
        return false;
    }
    const auto left_begin =
        reinterpret_cast<std::uintptr_t>(left.data());
    const auto right_begin =
        reinterpret_cast<std::uintptr_t>(right.data());
    if (left.size() >
            std::numeric_limits<std::uintptr_t>::max() -
                left_begin ||
        right.size() >
            std::numeric_limits<std::uintptr_t>::max() -
                right_begin) {
        return true;
    }
    const auto left_end = left_begin + left.size();
    const auto right_end = right_begin + right.size();
    return left_begin < right_end && right_begin < left_end;
}

std::int64_t deadline_slack(
    std::uint64_t deadline,
    std::uint64_t finish) noexcept {
    if (finish <= deadline) {
        const auto magnitude = deadline - finish;
        return magnitude >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(magnitude);
    }
    const auto magnitude = finish - deadline;
    if (magnitude >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
}

class SteadyRuntimeClock final : public rt::RuntimeClock {
public:
    SteadyRuntimeClock() noexcept
        : origin_(std::chrono::steady_clock::now()) {}

    std::uint64_t now_ns() noexcept override {
        const auto elapsed = std::chrono::steady_clock::now() - origin_;
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

    rt::Status sleep_until_ns(
        std::uint64_t absolute_ns) noexcept override {
        if (absolute_ns >
            static_cast<std::uint64_t>(
                std::chrono::nanoseconds::max().count())) {
            return rt::Status::clock_failure;
        }
        const auto offset =
            std::chrono::ceil<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(
                    static_cast<std::chrono::nanoseconds::rep>(
                        absolute_ns)));
        if (offset < std::chrono::steady_clock::duration::zero() ||
            origin_ >
                std::chrono::steady_clock::time_point::max() -
                    offset) {
            return rt::Status::clock_failure;
        }
        std::this_thread::sleep_until(
            origin_ + offset);
        return rt::Status::ok;
    }

    bool supports_absolute_sleep() const noexcept override {
        return std::chrono::steady_clock::is_steady;
    }

private:
    std::chrono::steady_clock::time_point origin_;
};

bool parse_u64(
    std::string_view value,
    std::uint64_t& parsed) noexcept {
    if (value.empty()) {
        return false;
    }

    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    return true;
}

bool parse_size(std::string_view value, std::size_t& parsed) noexcept {
    std::uint64_t raw = 0;
    if (!parse_u64(value, raw) ||
        raw > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    parsed = static_cast<std::size_t>(raw);
    return true;
}

bool identifier_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '.' || value == '_' || value == ':' ||
           value == '/' || value == '@' || value == '-';
}

bool valid_identifier(
    const std::array<
        char,
        rt::observability_identifier_capacity>& identifier) noexcept {
    std::size_t length = 0;
    while (length < identifier.size() &&
           identifier[length] != '\0') {
        if (!identifier_character(identifier[length])) {
            return false;
        }
        ++length;
    }
    return length != 0 && length < identifier.size();
}

template <std::size_t Capacity>
bool valid_identifier(
    const char (&identifier)[Capacity]) noexcept {
    std::size_t length = 0;
    while (length < Capacity && identifier[length] != '\0') {
        if (!identifier_character(identifier[length])) {
            return false;
        }
        ++length;
    }
    if (length == 0 || length == Capacity) {
        return false;
    }
    return std::all_of(
        identifier + length + 1,
        identifier + Capacity,
        [](char value) { return value == '\0'; });
}

template <std::size_t Capacity>
bool reserved_zero(const std::uint64_t (&values)[Capacity]) noexcept {
    return std::all_of(
        std::begin(values),
        std::end(values),
        [](std::uint64_t value) { return value == 0; });
}

bool set_identifier(
    std::array<
        char,
        rt::observability_identifier_capacity>& identifier,
    std::string_view value) noexcept {
    if (value.empty() || value.size() >= identifier.size()) {
        return false;
    }
    for (const char character : value) {
        if (!identifier_character(character)) {
            return false;
        }
    }
    identifier.fill('\0');
    std::copy(value.begin(), value.end(), identifier.begin());
    return true;
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        hash_byte(
            hash,
            static_cast<std::uint8_t>(
                (value >> (byte * 8u)) & 0xffu));
    }
}

void hash_bytes(
    std::uint64_t& hash,
    std::span<const std::byte> bytes) noexcept {
    hash_u64(hash, bytes.size());
    for (const auto value : bytes) {
        hash_byte(hash, static_cast<std::uint8_t>(value));
    }
}

void hash_string(
    std::uint64_t& hash,
    std::string_view value) noexcept {
    hash_bytes(hash, std::as_bytes(std::span(value)));
}

std::string_view identifier_view(
    const std::array<
        char,
        rt::observability_identifier_capacity>& identifier) noexcept {
    const auto end =
        std::find(identifier.begin(), identifier.end(), '\0');
    return std::string_view(
        identifier.data(),
        static_cast<std::size_t>(end - identifier.begin()));
}

std::uint64_t config_identifier(
    const rt::RuntimeConfig& config) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_u64(hash, rt::runtime_config_schema_version);
    hash_u64(hash, config.callback_capacity);
    hash_u64(hash, config.scratch_bytes);
    hash_u64(hash, config.trace_capacity);
    hash_u64(
        hash,
        static_cast<std::uint64_t>(config.numerical_mode));
    hash_u64(
        hash,
        static_cast<std::uint64_t>(config.executor_policy));
    hash_u64(hash, config.worker_count);
    hash_u64(hash, config.executor_queue_capacity);
    hash_u64(hash, config.scratch_alignment);
    hash_u64(hash, config.task_scratch_bytes);
    hash_u64(hash, config.task_scratch_slots);
    hash_u64(hash, config.memory_budget_bytes);
    hash_u64(
        hash,
        static_cast<std::uint64_t>(config.overload_policy));
    hash_u64(hash, config.watchdog_timeout_ns);
    hash_u64(
        hash,
        config.watchdog_max_degradation_level);
    hash_u64(
        hash,
        static_cast<std::uint64_t>(
            config.platform_preflight_mode));
    hash_u64(
        hash,
        static_cast<std::uint64_t>(
            config.determinism_tier));
    hash_u64(hash, config.state_capacity);
    hash_u64(hash, config.snapshot_max_bytes);
    hash_u64(hash, config.replay_input_capacity);
    hash_u64(hash, config.input_log_max_bytes);
    hash_u64(hash, config.device_backend_capacity);
    hash_u64(hash, config.device_buffer_capacity);
    hash_u64(hash, config.device_outstanding_capacity);
    hash_u64(hash, config.device_completion_batch);
    hash_string(hash, identifier_view(config.workload_id));
    return hash;
}

rt::Status validate_config(const rt::RuntimeConfig& config) noexcept {
    if (config.callback_capacity == 0 ||
        config.callback_capacity > kMaxCallbacks) {
        return rt::Status::invalid_config;
    }
    if (config.scratch_bytes > kMaxScratchBytes ||
        config.trace_capacity > kMaxTraceEvents) {
        return rt::Status::invalid_config;
    }
    if (config.worker_count == 0 ||
        config.worker_count > kMaxWorkers ||
        config.executor_queue_capacity < 2 ||
        config.executor_queue_capacity > kMaxExecutorQueueCapacity ||
        (config.executor_queue_capacity &
         (config.executor_queue_capacity - 1)) != 0) {
        return rt::Status::invalid_config;
    }
    if (config.scratch_alignment < alignof(std::max_align_t) ||
        config.scratch_alignment > kMaxScratchAlignment ||
        (config.scratch_alignment &
         (config.scratch_alignment - 1)) != 0 ||
        config.task_scratch_bytes > kMaxTaskScratchBytes ||
        config.task_scratch_slots == 0 ||
        config.task_scratch_slots > kMaxTaskScratchSlots ||
        config.memory_budget_bytes == 0 ||
        config.memory_budget_bytes > kMaxMemoryBudgetBytes ||
        config.watchdog_timeout_ns > kMaxWatchdogTimeoutNs ||
        config.watchdog_max_degradation_level >
            kMaxDegradationLevel ||
        config.state_capacity > kMaxRegisteredStates ||
        config.snapshot_max_bytes <
            rt::detail::checkpoint_header_size ||
        config.snapshot_max_bytes > kMaxArtifactBytes ||
        config.replay_input_capacity > kMaxReplayInputs ||
        config.input_log_max_bytes <
            rt::detail::input_log_header_size ||
        config.input_log_max_bytes > kMaxArtifactBytes ||
        config.device_backend_capacity == 0 ||
        config.device_backend_capacity > kMaxDeviceBackends ||
        config.device_buffer_capacity > kMaxDeviceBuffers ||
        config.device_outstanding_capacity == 0 ||
        config.device_outstanding_capacity > kMaxDeviceOutstanding ||
        config.device_completion_batch == 0 ||
        config.device_completion_batch > kMaxDeviceCompletionBatch ||
        config.device_completion_batch >
            config.device_outstanding_capacity ||
        !valid_identifier(config.workload_id)) {
        return rt::Status::invalid_config;
    }
    switch (config.numerical_mode) {
    case rt::NumericalMode::precise:
    case rt::NumericalMode::fused_multiply_add:
        break;
    default:
        return rt::Status::invalid_config;
    }
    switch (config.executor_policy) {
    case rt::ExecutorPolicy::static_deterministic:
    case rt::ExecutorPolicy::bounded_throughput:
    case rt::ExecutorPolicy::host_adapter:
        break;
    default:
        return rt::Status::invalid_config;
    }
    switch (config.overload_policy) {
    case rt::OverloadPolicy::reject_submission:
    case rt::OverloadPolicy::fail_frame:
        break;
    default:
        return rt::Status::invalid_config;
    }
    switch (config.platform_preflight_mode) {
    case rt::PlatformPreflightMode::disabled:
    case rt::PlatformPreflightMode::strict:
        break;
    default:
        return rt::Status::invalid_config;
    }
    switch (config.determinism_tier) {
    case rt::DeterminismTier::unspecified:
        break;
    case rt::DeterminismTier::schedule_independent:
        if (config.executor_policy !=
                rt::ExecutorPolicy::static_deterministic ||
            config.watchdog_timeout_ns != 0) {
            return rt::Status::invalid_config;
        }
        break;
    case rt::DeterminismTier::reproducible_build:
    case rt::DeterminismTier::portable_deterministic:
        return rt::Status::invalid_config;
    default:
        return rt::Status::invalid_config;
    }
    return rt::Status::ok;
}

} // namespace

namespace rt {

Capabilities query_capabilities() noexcept {
    // M11 adds the borrowed host job-system executor policy.
    return {
        true, true, true, true, true, true,
        true, true, true, true, true};
}

const char* status_message(Status status) noexcept {
    switch (status) {
    case Status::ok:
        return "ok";
    case Status::invalid_argument:
        return "invalid argument";
    case Status::invalid_state:
        return "invalid runtime state";
    case Status::invalid_config:
        return "invalid runtime configuration";
    case Status::capacity_exceeded:
        return "configured capacity exceeded";
    case Status::callback_failed:
        return "user callback failed";
    case Status::resource_exhausted:
        return "resource allocation failed";
    case Status::internal_error:
        return "internal runtime error";
    case Status::invalid_handle:
        return "invalid graph handle";
    case Status::graph_cycle:
        return "graph contains a cycle";
    case Status::resource_conflict:
        return "unordered conflicting resource access";
    case Status::queue_full:
        return "executor queue is full";
    case Status::scratch_exhausted:
        return "task scratch plan is exhausted";
    case Status::platform_preflight_failed:
        return "strict platform preflight failed";
    case Status::clock_failure:
        return "runtime clock operation failed";
    case Status::invalid_artifact:
        return "checkpoint or input-log artifact is invalid";
    case Status::incompatible_artifact:
        return "checkpoint or input-log artifact is incompatible";
    case Status::device_queue_full:
        return "device submission queue is full";
    case Status::device_timeout:
        return "device submission timed out";
    case Status::device_error:
        return "device backend error";
    case Status::device_lost:
        return "device was lost";
    case Status::device_canceled:
        return "device submission was canceled";
    case Status::device_reset_required:
        return "device reset is required";
    case Status::incompatible_abi:
        return "C ABI version or layout is incompatible";
    }
    return "unknown runtime status";
}

Status set_runtime_config_value(
    RuntimeConfig& config,
    std::string_view key,
    std::string_view value) noexcept {
    RuntimeConfig candidate = config;
    std::size_t parsed = 0;
    std::uint64_t parsed_u64 = 0;

    if (key == "callback_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.callback_capacity = parsed;
    } else if (key == "scratch_bytes") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.scratch_bytes = parsed;
    } else if (key == "trace_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.trace_capacity = parsed;
    } else if (key == "numerical_mode") {
        if (value == "precise") {
            candidate.numerical_mode = NumericalMode::precise;
        } else if (value == "fused_multiply_add") {
            candidate.numerical_mode = NumericalMode::fused_multiply_add;
        } else {
            return Status::invalid_config;
        }
    } else if (key == "executor_policy") {
        if (value == "static_deterministic") {
            candidate.executor_policy =
                ExecutorPolicy::static_deterministic;
        } else if (value == "bounded_throughput") {
            candidate.executor_policy =
                ExecutorPolicy::bounded_throughput;
        } else if (value == "host_adapter") {
            candidate.executor_policy =
                ExecutorPolicy::host_adapter;
        } else {
            return Status::invalid_config;
        }
    } else if (key == "worker_count") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.worker_count = parsed;
    } else if (key == "executor_queue_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.executor_queue_capacity = parsed;
    } else if (key == "scratch_alignment") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.scratch_alignment = parsed;
    } else if (key == "task_scratch_bytes") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.task_scratch_bytes = parsed;
    } else if (key == "task_scratch_slots") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.task_scratch_slots = parsed;
    } else if (key == "memory_budget_bytes") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.memory_budget_bytes = parsed;
    } else if (key == "overload_policy") {
        if (value == "reject_submission") {
            candidate.overload_policy =
                OverloadPolicy::reject_submission;
        } else if (value == "fail_frame") {
            candidate.overload_policy =
                OverloadPolicy::fail_frame;
        } else {
            return Status::invalid_config;
        }
    } else if (key == "watchdog_timeout_ns") {
        if (!parse_u64(value, parsed_u64)) {
            return Status::invalid_config;
        }
        candidate.watchdog_timeout_ns = parsed_u64;
    } else if (key == "watchdog_max_degradation_level") {
        if (!parse_size(value, parsed) ||
            parsed > std::numeric_limits<std::uint32_t>::max()) {
            return Status::invalid_config;
        }
        candidate.watchdog_max_degradation_level =
            static_cast<std::uint32_t>(parsed);
    } else if (key == "platform_preflight_mode") {
        if (value == "disabled") {
            candidate.platform_preflight_mode =
                PlatformPreflightMode::disabled;
        } else if (value == "strict") {
            candidate.platform_preflight_mode =
                PlatformPreflightMode::strict;
        } else {
            return Status::invalid_config;
        }
    } else if (key == "determinism_tier") {
        if (value == "unspecified" || value == "d0") {
            candidate.determinism_tier =
                DeterminismTier::unspecified;
        } else if (
            value == "schedule_independent" ||
            value == "d1") {
            candidate.determinism_tier =
                DeterminismTier::schedule_independent;
        } else {
            return Status::invalid_config;
        }
    } else if (key == "state_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.state_capacity = parsed;
    } else if (key == "snapshot_max_bytes") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.snapshot_max_bytes = parsed;
    } else if (key == "replay_input_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.replay_input_capacity = parsed;
    } else if (key == "input_log_max_bytes") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.input_log_max_bytes = parsed;
    } else if (key == "device_backend_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.device_backend_capacity = parsed;
    } else if (key == "device_buffer_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.device_buffer_capacity = parsed;
    } else if (key == "device_outstanding_capacity") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.device_outstanding_capacity = parsed;
    } else if (key == "device_completion_batch") {
        if (!parse_size(value, parsed)) {
            return Status::invalid_config;
        }
        candidate.device_completion_batch = parsed;
    } else if (key == "workload_id") {
        if (!set_identifier(candidate.workload_id, value)) {
            return Status::invalid_config;
        }
    } else {
        return Status::invalid_config;
    }

    const auto status = validate_config(candidate);
    if (status == Status::ok) {
        config = candidate;
    }
    return status;
}

double NumericalPolicy::multiply_add(
    double a,
    double b,
    double c) const noexcept {
    if (mode_ == NumericalMode::fused_multiply_add) {
        return std::fma(a, b, c);
    }

    volatile double product = a * b;
    return product + c;
}

struct Runtime::Impl {
    class SpinGuard {
    public:
        explicit SpinGuard(std::atomic_flag& lock) noexcept
            : lock_(lock) {
            while (lock_.test_and_set(std::memory_order_acquire)) {
                rt::cpu_relax();
            }
        }

        ~SpinGuard() {
            lock_.clear(std::memory_order_release);
        }

        SpinGuard(const SpinGuard&) = delete;
        SpinGuard& operator=(const SpinGuard&) = delete;

    private:
        std::atomic_flag& lock_;
    };

    enum class PhaseKind : std::uint8_t {
        cpu,
        device,
    };

    struct RegisteredCallback {
        std::string name;
        PhaseKind kind = PhaseKind::cpu;
        FrameCallback callback = nullptr;
        DeviceCommandCallback device_callback = nullptr;
        void* user_data = nullptr;
        std::uint32_t device_backend_index = 0;
    };

    struct RegisteredResource {
        std::string name;
    };

    struct RegisteredState {
        std::array<char, replay_identifier_capacity> name{};
        std::uint32_t schema_version = 0;
        std::span<std::byte> storage{};
    };

    explicit Impl(
        RuntimeClock* injected_clock,
        PlatformPreflightProbe* injected_preflight)
        : graph_owner(next_graph_owner()),
          clock(injected_clock ? injected_clock : &owned_clock),
          preflight(
              injected_preflight
                  ? injected_preflight
                  : &owned_preflight) {
        thread_policy_provider = &owned_thread_policy_provider;
        error[0] = '\0';
    }

    ~Impl() {
        if (devices) {
            (void)devices->stop();
        }
        if (executor) {
            executor->stop();
        }
        watchdog.stop();
    }

    void clear_error() noexcept {
        SpinGuard guard(error_lock);
        error[0] = '\0';
    }

    Status fail(Status status, const char* message) noexcept {
        SpinGuard guard(error_lock);
        if (!message) {
            message = status_message(status);
        }
        std::snprintf(error.data(), error.size(), "%s", message);
        return status;
    }

    Status fail_callback(std::size_t index) noexcept {
        SpinGuard guard(error_lock);
        const char* name = index < callbacks.size()
            ? callbacks[index].name.c_str()
            : "<unknown>";
        std::snprintf(
            error.data(),
            error.size(),
            "callback %zu ('%s') failed",
            index,
            name);
        return Status::callback_failed;
    }

    [[nodiscard]] bool valid_phase(PhaseHandle phase) const noexcept {
        return phase.valid() &&
               phase.owner() == graph_owner &&
               phase.index() < callbacks.size();
    }

    [[nodiscard]] bool valid_resource(ResourceHandle resource) const noexcept {
        return resource.valid() &&
               resource.owner() == graph_owner &&
               resource.index() < resources.size();
    }

    [[nodiscard]] bool valid_device_backend(
        DeviceBackendHandle backend) const noexcept {
        return backend.valid() &&
               backend.owner() == graph_owner &&
               backend.index() < device_backends.size();
    }

    [[nodiscard]] bool valid_device_buffer(
        DeviceBufferHandle buffer) const noexcept {
        return buffer.valid() &&
               buffer.owner() == graph_owner &&
               buffer.index() < device_buffers.size();
    }

    [[nodiscard]] std::uint64_t compute_graph_id() const noexcept {
        std::uint64_t hash = kFnvOffset;
        hash_u64(hash, 1);
        hash_u64(hash, callbacks.size());
        for (const auto& callback : callbacks) {
            hash_string(hash, callback.name);
            hash_u64(
                hash,
                static_cast<std::uint64_t>(callback.kind));
            if (callback.kind == PhaseKind::device) {
                hash_u64(hash, callback.device_backend_index);
            }
        }
        hash_u64(hash, device_backends.size());
        for (const auto& backend : device_backends) {
            hash_string(hash, backend.name);
            hash_string(
                hash,
                std::string_view(
                    backend.capabilities.backend_id,
                    std::char_traits<char>::length(
                        backend.capabilities.backend_id)));
        }
        hash_u64(hash, device_buffers.size());
        for (const auto& buffer : device_buffers) {
            hash_string(
                hash,
                std::string_view(
                    buffer.name.data(),
                    std::char_traits<char>::length(
                        buffer.name.data())));
            hash_u64(hash, buffer.backend_index);
            hash_u64(hash, buffer.storage.size());
            hash_u64(hash, buffer.flags);
        }
        hash_u64(hash, resources.size());
        for (const auto& resource : resources) {
            hash_string(hash, resource.name);
        }
        hash_u64(hash, dependencies.size());
        for (const auto& dependency : dependencies) {
            hash_u64(hash, dependency.prerequisite.index());
            hash_u64(hash, dependency.dependent.index());
        }
        hash_u64(hash, resource_accesses.size());
        for (const auto& access : resource_accesses) {
            hash_u64(hash, access.phase.index());
            hash_u64(hash, access.resource.index());
            hash_u64(
                hash,
                static_cast<std::uint64_t>(access.access));
        }
        return hash;
    }

    [[nodiscard]] std::uint64_t
    compute_state_schema_id() const noexcept {
        std::uint64_t hash = kFnvOffset;
        hash_u64(hash, checkpoint_schema_version);
        hash_u64(hash, states.size());
        for (const auto& registered_state : states) {
            hash_string(hash, identifier_view(registered_state.name));
            hash_u64(hash, registered_state.schema_version);
            hash_u64(hash, registered_state.storage.size());
        }
        return hash;
    }

    [[nodiscard]] std::uint64_t compute_replay_id(
        std::uint64_t resolved_graph_id,
        std::uint64_t resolved_state_schema_id) const noexcept {
        std::uint64_t hash = kFnvOffset;
        hash_u64(hash, checkpoint_schema_version);
        hash_u64(
            hash,
            static_cast<std::uint64_t>(
                config.determinism_tier));
        if (config.determinism_tier ==
            DeterminismTier::unspecified) {
            hash_u64(hash, config_identifier(config));
        } else {
            // Operational capacities and worker_count are deliberately
            // excluded from D1 compatibility. Semantic callback-visible
            // choices remain part of the identity.
            hash_u64(
                hash,
                static_cast<std::uint64_t>(
                    config.numerical_mode));
            hash_u64(hash, config.scratch_bytes);
            hash_u64(hash, config.scratch_alignment);
            hash_u64(hash, config.task_scratch_bytes);
            hash_u64(
                hash,
                static_cast<std::uint64_t>(
                    config.overload_policy));
        }
        hash_u64(hash, resolved_graph_id);
        hash_u64(hash, resolved_state_schema_id);
        hash_string(hash, identifier_view(config.workload_id));
        return hash;
    }

    static bool provide_state(
        void* context,
        std::size_t index,
        detail::StateWriteView& output) noexcept {
        auto& self = *static_cast<Impl*>(context);
        if (index >= self.states.size()) {
            output = {};
            return false;
        }
        const auto& state = self.states[index];
        output.name = identifier_view(state.name);
        output.schema_version = state.schema_version;
        output.payload = std::as_bytes(state.storage);
        return true;
    }

    [[nodiscard]] std::uint64_t state_hash() const noexcept {
        std::uint64_t hash = kFnvOffset;
        const auto append =
            [&hash](std::span<const std::byte> bytes) {
                for (const auto value : bytes) {
                    hash ^= static_cast<std::uint8_t>(value);
                    hash *= kFnvPrime;
                }
            };
        for (const auto& registered_state : states) {
            std::array<
                std::byte,
                detail::checkpoint_record_header_size> header{};
            const auto name = identifier_view(registered_state.name);
            std::memcpy(
                header.data(),
                name.data(),
                name.size());
            store_u32_le(
                header,
                64,
                registered_state.schema_version);
            store_u64_le(
                header,
                72,
                registered_state.storage.size());
            store_u64_le(
                header,
                80,
                detail::artifact_checksum(
                    std::as_bytes(registered_state.storage)));
            append(header);
            append(std::as_bytes(registered_state.storage));
        }
        return hash;
    }

    Status fail_compile(
        Status status,
        const detail::GraphCompileDiagnostic& diagnostic) noexcept {
        SpinGuard guard(error_lock);
        if (status == Status::graph_cycle) {
            const char* name = valid_phase(diagnostic.first_phase)
                ? callbacks[diagnostic.first_phase.index()].name.c_str()
                : "<unknown>";
            std::snprintf(
                error.data(),
                error.size(),
                "dependency cycle includes phase %u ('%s')",
                static_cast<unsigned>(diagnostic.first_phase.index()),
                name);
            return status;
        }
        if (status == Status::resource_conflict) {
            const char* first = valid_phase(diagnostic.first_phase)
                ? callbacks[diagnostic.first_phase.index()].name.c_str()
                : "<unknown>";
            const char* second = valid_phase(diagnostic.second_phase)
                ? callbacks[diagnostic.second_phase.index()].name.c_str()
                : "<unknown>";
            const char* resource = valid_resource(diagnostic.resource)
                ? resources[diagnostic.resource.index()].name.c_str()
                : "<unknown>";
            std::snprintf(
                error.data(),
                error.size(),
                "resource '%s' has unordered conflicting access by phases "
                "'%s' and '%s'; add a dependency path",
                resource,
                first,
                second);
            return status;
        }
        if (status == Status::invalid_handle) {
            std::snprintf(
                error.data(),
                error.size(),
                "%s",
                "graph contains an invalid phase or resource handle");
            return status;
        }
        std::snprintf(
            error.data(),
            error.size(),
            "%s",
            status_message(status));
        return status;
    }

    [[nodiscard]] std::uint64_t clock_now() noexcept {
        SpinGuard guard(clock_lock);
        return clock->now_ns();
    }

    [[nodiscard]] Status clock_sleep_until(
        std::uint64_t absolute_ns) noexcept {
        SpinGuard guard(clock_lock);
        return clock->sleep_until_ns(absolute_ns);
    }

    Status fail_preflight() noexcept {
        for (std::size_t index = 0;
             index < preflight_report.check_count;
             ++index) {
            const auto& check = preflight_report.checks[index];
            if (check.status != PlatformCheckStatus::passed) {
                return fail(
                    Status::platform_preflight_failed,
                    check.message[0] != '\0'
                        ? check.message.data()
                        : "strict platform preflight prerequisite failed");
            }
        }
        return fail(
            Status::platform_preflight_failed,
            "strict platform preflight report is incomplete or contains "
            "duplicate prerequisites");
    }

    void record(
        RuntimeTraceEventType type,
        Status status,
        std::uint64_t timestamp_ns,
        std::uint64_t frame_index,
        std::size_t callback_index = kNoCallback,
        RuntimeTraceProducer producer =
            RuntimeTraceProducer::host,
        std::size_t worker_index = kNoWorker,
        std::uint64_t value = 0) noexcept {
        switch (type) {
        case RuntimeTraceEventType::step_begin:
            telemetry_counters.increment(
                RuntimeMetricId::frames_started);
            break;
        case RuntimeTraceEventType::step_end:
            telemetry_counters.increment(
                RuntimeMetricId::frames_completed);
            if (status != Status::ok) {
                telemetry_counters.increment(
                    RuntimeMetricId::frames_failed);
            }
            break;
        case RuntimeTraceEventType::callback_begin:
            telemetry_counters.increment(
                RuntimeMetricId::callbacks_started);
            break;
        case RuntimeTraceEventType::callback_end:
            telemetry_counters.increment(
                RuntimeMetricId::callbacks_completed);
            if (status != Status::ok) {
                telemetry_counters.increment(
                    RuntimeMetricId::callback_failures);
            }
            break;
        case RuntimeTraceEventType::watchdog_fired:
            telemetry_counters.increment(
                RuntimeMetricId::watchdog_events);
            break;
        case RuntimeTraceEventType::degradation_applied:
            telemetry_counters.increment(
                RuntimeMetricId::degradation_events);
            break;
        case RuntimeTraceEventType::periodic_release:
            telemetry_counters.increment(
                RuntimeMetricId::periodic_releases);
            break;
        case RuntimeTraceEventType::periodic_wake:
            telemetry_counters.increment(
                RuntimeMetricId::periodic_wakes);
            break;
        case RuntimeTraceEventType::finalized:
        case RuntimeTraceEventType::started:
        case RuntimeTraceEventType::stopped:
        case RuntimeTraceEventType::device_submitted:
        case RuntimeTraceEventType::device_completed:
        case RuntimeTraceEventType::device_reset:
            break;
        }

        if (!telemetry) {
            return;
        }
        RuntimeTraceEvent event;
        event.type = type;
        event.status = status;
        event.producer = producer;
        event.timestamp_ns = timestamp_ns;
        event.frame_index = frame_index;
        event.callback_index =
            callback_index == kNoCallback
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>(callback_index);
        event.worker_index =
            worker_index == kNoWorker
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>(worker_index);
        event.value = value;
        (void)telemetry->emit(event);
    }

    [[nodiscard]] std::array<
        std::uint64_t,
        runtime_metric_count> metric_values() const noexcept {
        std::array<std::uint64_t, runtime_metric_count> values{};
        for (std::size_t index = 0;
             index < values.size();
             ++index) {
            values[index] = telemetry_counters.load(
                static_cast<RuntimeMetricId>(index));
        }

        const auto set =
            [&values](
                RuntimeMetricId id,
                std::uint64_t value) {
                values[static_cast<std::size_t>(id)] = value;
            };
        if (telemetry) {
            set(
                RuntimeMetricId::trace_events_emitted,
                telemetry->emitted());
            set(
                RuntimeMetricId::trace_events_overwritten,
                telemetry->overwritten());
            set(
                RuntimeMetricId::trace_events_dropped,
                telemetry->dropped());
        }
        if (executor) {
            const auto stats = executor->stats();
            set(
                RuntimeMetricId::executor_submitted_tasks,
                stats.submitted_tasks);
            set(
                RuntimeMetricId::executor_local_executions,
                stats.local_executions);
            set(
                RuntimeMetricId::executor_steal_attempts,
                stats.steal_attempts);
            set(
                RuntimeMetricId::executor_successful_steals,
                stats.successful_steals);
            set(
                RuntimeMetricId::executor_queue_rejections,
                stats.queue_full_rejections);
            set(
                RuntimeMetricId::executor_scratch_exhaustions,
                stats.scratch_exhaustions);
            set(
                RuntimeMetricId::executor_worker_starts,
                stats.worker_starts);
        }
        if (devices) {
            const auto stats = devices->stats();
            set(RuntimeMetricId::device_submissions, stats.submissions);
            set(RuntimeMetricId::device_completions, stats.completions);
            set(RuntimeMetricId::device_failures, stats.failures);
            set(
                RuntimeMetricId::device_queue_rejections,
                stats.queue_rejections);
            set(RuntimeMetricId::device_timeouts, stats.timeouts);
            set(RuntimeMetricId::device_losses, stats.losses);
            set(RuntimeMetricId::device_resets, stats.resets);
            set(
                RuntimeMetricId::device_service_polls,
                stats.service_polls);
            set(RuntimeMetricId::device_outstanding, stats.outstanding);
            set(
                RuntimeMetricId::device_service_starts,
                stats.service_starts);
        }
        set(
            RuntimeMetricId::degradation_level,
            degradation_level.load(std::memory_order_acquire));
        return values;
    }

    static void observe_device_event(
        void* opaque,
        const detail::DeviceEvent& event) noexcept {
        auto& self = *static_cast<Impl*>(opaque);
        RuntimeTraceEventType type =
            RuntimeTraceEventType::device_completed;
        switch (event.kind) {
        case detail::DeviceEventKind::submitted:
            type = RuntimeTraceEventType::device_submitted;
            break;
        case detail::DeviceEventKind::completed:
            type = RuntimeTraceEventType::device_completed;
            break;
        case detail::DeviceEventKind::reset:
            type = RuntimeTraceEventType::device_reset;
            break;
        }
        self.record(
            type,
            event.status,
            self.clock_now(),
            event.frame_index,
            event.phase_index,
            event.producer,
            event.worker_index,
            event.kind == detail::DeviceEventKind::reset
                ? event.backend_index
                : event.submission_id);
    }

    static detail::PhaseTaskDispatch run_phase(
        void* opaque,
        std::uint32_t phase_index,
        const TaskContext& task_context) {
        auto& self = *static_cast<Impl*>(opaque);
        const auto index = static_cast<std::size_t>(phase_index);
        if (index >= self.callbacks.size() || self.active_frame == nullptr) {
            return {Status::callback_failed, false};
        }

        auto& callback = self.callbacks[index];
        self.record(
            RuntimeTraceEventType::callback_begin,
            Status::ok,
            self.clock_now(),
            self.active_frame->frame_index,
            index,
            RuntimeTraceProducer::worker,
            task_context.worker_index(),
            task_context.task_index());

        std::span<std::byte> phase_scratch;
        if (self.config.scratch_bytes != 0) {
            phase_scratch = std::span<std::byte>(
                self.phase_scratch.data() +
                    (index *
                     self.finalized_memory_plan.phase_scratch_stride),
                self.config.scratch_bytes);
        }
        CallbackResult result = CallbackResult::error;
        Status status = Status::callback_failed;
        bool pending = false;
        if (callback.kind == PhaseKind::cpu) {
            CallbackContext callback_context{
                *self.active_frame,
                phase_scratch,
                self.numerics,
                task_context,
                self.degradation_level.load(std::memory_order_acquire),
            };
            try {
                result = callback.callback(
                    callback.user_data,
                    callback_context);
            } catch (...) {
                result = CallbackResult::error;
            }
            status = result == CallbackResult::ok
                ? Status::ok
                : Status::callback_failed;
        } else {
            DeviceCallbackContext callback_context{
                *self.active_frame,
                phase_scratch,
                self.numerics,
                task_context,
                self.degradation_level.load(std::memory_order_acquire),
            };
            auto submission = make_device_submission();
            try {
                result = callback.device_callback(
                    callback.user_data,
                    callback_context,
                    submission);
            } catch (...) {
                result = CallbackResult::error;
            }
            if (result == CallbackResult::ok && self.devices) {
                std::uint64_t submission_id = 0;
                status = self.devices->submit(
                    callback.device_backend_index,
                    index,
                    task_context.worker_index(),
                    self.active_frame->frame_index,
                    submission,
                    submission_id);
                pending = status == Status::ok;
            }
        }

        self.record(
            RuntimeTraceEventType::callback_end,
            status,
            self.clock_now(),
            self.active_frame->frame_index,
            index,
            RuntimeTraceProducer::worker,
            task_context.worker_index(),
            task_context.task_index());
        return {status, pending};
    }

    std::uint32_t graph_owner;
    SteadyRuntimeClock owned_clock;
    RuntimeClock* clock;
    detail::NativePlatformPreflightProbe owned_preflight;
    PlatformPreflightProbe* preflight;
    detail::NativeThreadPolicyProvider owned_thread_policy_provider;
    ThreadPolicyProvider* thread_policy_provider = nullptr;
    detail::ThreadPolicyTransaction thread_policy_transaction;
    RuntimeConfig config{};
    HostExecutorAdapter host_executor{};
    bool host_executor_set = false;
    RuntimeState state = RuntimeState::configuring;
    NumericalPolicy numerics{};
    std::vector<RegisteredCallback> callbacks;
    std::vector<detail::DeviceBackendSpec> device_backends;
    std::vector<detail::DeviceBufferSpec> device_buffers;
    std::vector<RegisteredResource> resources;
    std::vector<RegisteredState> states;
    std::vector<detail::GraphDependency> dependencies;
    std::vector<detail::GraphResourceAccess> resource_accesses;
    std::vector<ThreadPolicyRequest> thread_policy_requests;
    std::vector<MemoryPolicyRequest> memory_policy_requests;
    std::vector<ThreadPolicyReport> thread_policy_reports;
    std::vector<MemoryRegionPolicyReport> memory_policy_reports;
    CpuMemoryPolicySummary cpu_memory_policy_summary{};
    std::vector<PhaseHandle> compiled_order;
    detail::AlignedStorage phase_scratch;
    std::unique_ptr<detail::TelemetryRing> telemetry;
    detail::TelemetryCounters telemetry_counters;
    ObservabilityMetadata observability{};
    std::uint64_t graph_id = 0;
    std::uint64_t state_schema_id = 0;
    std::uint64_t replay_id = 0;
    std::uint64_t telemetry_epoch_ns = 0;
    std::uint64_t metric_snapshot_sequence = 0;
    std::unique_ptr<detail::Executor> executor;
    std::unique_ptr<detail::DeviceManager> devices;
    detail::WatchdogMonitor watchdog;
    MemoryPlan finalized_memory_plan{};
    PlatformPreflightReport preflight_report{};
    bool preflight_report_available = false;
    std::atomic<bool> in_step{false};
    std::atomic<bool> in_periodic_run{false};
    std::atomic<bool> periodic_dispatch{false};
    std::atomic<bool> in_replay{false};
    std::atomic<bool> replay_dispatch{false};
    std::atomic<std::uint32_t> degradation_level{0};
    bool watchdog_started = false;
    bool stop_pending = false;
    const HostFrameContext* active_frame = nullptr;
    std::array<char, 256> error{};
    mutable std::atomic_flag error_lock = ATOMIC_FLAG_INIT;
    std::atomic_flag clock_lock = ATOMIC_FLAG_INIT;
};

Runtime::Runtime()
    : impl_(std::make_unique<Impl>(nullptr, nullptr)) {}

Runtime::Runtime(RuntimeClock& clock)
    : impl_(std::make_unique<Impl>(&clock, nullptr)) {}

Runtime::Runtime(
    RuntimeClock& clock,
    PlatformPreflightProbe& preflight)
    : impl_(std::make_unique<Impl>(&clock, &preflight)) {}

Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

Status Runtime::configure(const RuntimeConfig& config) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "configure requires configuring state");
    }
    if (validate_config(config) != Status::ok ||
        config.callback_capacity < impl_->callbacks.size() ||
        config.state_capacity < impl_->states.size() ||
        config.device_backend_capacity <
            impl_->device_backends.size() ||
        config.device_buffer_capacity <
            impl_->device_buffers.size()) {
        return impl_->fail(Status::invalid_config, nullptr);
    }
    impl_->config = config;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::set_cpu_memory_policy(
    const CpuMemoryPolicyRequest& policy) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "CPU and memory policy is frozen");
    }
    try {
        std::vector<ThreadPolicyRequest> thread_requests(
            policy.threads.begin(),
            policy.threads.end());
        std::vector<MemoryPolicyRequest> memory_requests(
            policy.memory_regions.begin(),
            policy.memory_regions.end());
        impl_->thread_policy_requests = std::move(thread_requests);
        impl_->memory_policy_requests = std::move(memory_requests);
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::set_thread_policy_provider(
    ThreadPolicyProvider& provider) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "thread policy provider requires configuring state");
    }
    const auto capabilities = provider.capabilities();
    if ((capabilities.thread_name &&
         capabilities.thread_name_capacity < 2) ||
        (!capabilities.thread_name &&
         capabilities.thread_name_capacity != 0)) {
        return impl_->fail(
            Status::invalid_argument,
            "thread policy provider capabilities are invalid");
    }
    impl_->thread_policy_provider = &provider;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::set_host_executor(
    const HostExecutorAdapter& adapter) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "host executor attachment is frozen");
    }
    if (adapter.worker_count == 0 ||
        adapter.worker_count > kMaxWorkers ||
        adapter.queue_capacity < 2 ||
        adapter.queue_capacity > kMaxExecutorQueueCapacity ||
        (adapter.queue_capacity & (adapter.queue_capacity - 1)) != 0 ||
        adapter.submit == nullptr ||
        adapter.try_execute_one == nullptr) {
        return impl_->fail(
            Status::invalid_argument,
            "host executor adapter is incomplete or has invalid capacities");
    }
    impl_->host_executor = adapter;
    impl_->host_executor_set = true;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::configure_key(
    std::string_view key,
    std::string_view value) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "configuration is frozen");
    }

    RuntimeConfig candidate = impl_->config;
    const auto status = set_runtime_config_value(candidate, key, value);
    if (status != Status::ok ||
        candidate.callback_capacity < impl_->callbacks.size() ||
        candidate.state_capacity < impl_->states.size() ||
        candidate.device_backend_capacity <
            impl_->device_backends.size() ||
        candidate.device_buffer_capacity <
            impl_->device_buffers.size()) {
        return impl_->fail(Status::invalid_config, "unknown or invalid configuration key/value");
    }
    impl_->config = candidate;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_callback(
    const CallbackRegistration& registration) noexcept {
    PhaseHandle ignored;
    return register_callback(registration, ignored);
}

Status Runtime::register_callback(
    const CallbackRegistration& registration,
    PhaseHandle& out_phase) noexcept {
    out_phase = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "callback registration is frozen");
    }
    if (registration.name.empty() || !registration.callback) {
        return impl_->fail(Status::invalid_argument, "callback name and function are required");
    }
    if (impl_->callbacks.size() >= impl_->config.callback_capacity) {
        return impl_->fail(Status::capacity_exceeded, nullptr);
    }
    const auto duplicate = std::find_if(
        impl_->callbacks.begin(),
        impl_->callbacks.end(),
        [&](const Impl::RegisteredCallback& callback) {
            return callback.name == registration.name;
        });
    if (duplicate != impl_->callbacks.end()) {
        return impl_->fail(Status::invalid_argument, "callback names must be unique");
    }

    try {
        const auto index = static_cast<std::uint32_t>(impl_->callbacks.size());
        impl_->callbacks.push_back(Impl::RegisteredCallback{
            std::string(registration.name),
            Impl::PhaseKind::cpu,
            registration.callback,
            nullptr,
            registration.user_data,
            0,
        });
        out_phase = PhaseHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_device_backend(
    const DeviceBackendRegistration& registration,
    DeviceBackendHandle& out_backend) noexcept {
    out_backend = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "device backend registration is frozen");
    }
    std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
    if (!set_identifier(name, registration.name)) {
        return impl_->fail(
            Status::invalid_argument,
            "device backend name must be a stable identifier");
    }
    if (impl_->device_backends.size() >=
        impl_->config.device_backend_capacity) {
        return impl_->fail(
            Status::capacity_exceeded,
            "configured device backend capacity exceeded");
    }
    const auto& api = registration.api;
    if (api.struct_size < sizeof(api) ||
        api.abi_version != RTFW_DEVICE_ABI_VERSION ||
        !api.instance ||
        !api.get_capabilities ||
        !api.initialize ||
        !api.register_buffer ||
        !api.unregister_buffer ||
        !api.submit ||
        !api.poll ||
        !api.cancel ||
        !api.get_health ||
        !api.reset ||
        !api.shutdown ||
        !reserved_zero(api.reserved)) {
        return impl_->fail(
            Status::invalid_argument,
            "device backend function table is malformed or incompatible");
    }
    const auto duplicate = std::find_if(
        impl_->device_backends.begin(),
        impl_->device_backends.end(),
        [&](const detail::DeviceBackendSpec& backend) {
            return backend.name == registration.name;
        });
    if (duplicate != impl_->device_backends.end()) {
        return impl_->fail(
            Status::invalid_argument,
            "device backend names must be unique");
    }

    rtfw_device_capabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    capabilities.abi_version = RTFW_DEVICE_ABI_VERSION;
    rtfw_device_status device_status =
        RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    try {
        device_status = api.get_capabilities(
            api.instance,
            &capabilities);
    } catch (...) {
        device_status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    }
    const auto status =
        detail::device_status_to_runtime(device_status);
    if (status != Status::ok) {
        return impl_->fail(
            status,
            "device backend capability query failed");
    }
    if (capabilities.struct_size < sizeof(capabilities) ||
        capabilities.abi_version != RTFW_DEVICE_ABI_VERSION ||
        capabilities.max_in_flight == 0 ||
        capabilities.max_registered_buffers == 0 ||
        capabilities.max_buffer_bytes == 0 ||
        capabilities.inline_payload_capacity <
            RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY ||
        capabilities.buffer_ref_capacity <
            RTFW_DEVICE_BUFFER_REF_CAPACITY ||
        capabilities.supports_cancel > 1 ||
        capabilities.supports_reset > 1 ||
        capabilities.deterministic_mock > 1 ||
        capabilities.reserved0 != 0 ||
        !valid_identifier(capabilities.backend_id) ||
        !reserved_zero(capabilities.reserved) ||
        capabilities.control_storage_bytes >
            std::numeric_limits<std::size_t>::max()) {
        return impl_->fail(
            Status::invalid_argument,
            "device backend reported malformed capabilities");
    }

    try {
        const auto index = static_cast<std::uint32_t>(
            impl_->device_backends.size());
        impl_->device_backends.push_back(
            detail::DeviceBackendSpec{
                std::string(registration.name),
                api,
                capabilities,
            });
        out_backend =
            DeviceBackendHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_device_buffer(
    const DeviceBufferRegistration& registration,
    DeviceBufferHandle& out_buffer) noexcept {
    out_buffer = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "device buffer registration is frozen");
    }
    if (!impl_->valid_device_backend(registration.backend)) {
        return impl_->fail(
            Status::invalid_handle,
            "device buffer references an invalid or foreign backend");
    }
    std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
    if (!set_identifier(name, registration.name) ||
        registration.storage.empty()) {
        return impl_->fail(
            Status::invalid_argument,
            "device buffer requires a stable name and non-empty storage");
    }
    constexpr auto allowed_flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    if (registration.flags == 0 ||
        (registration.flags & ~allowed_flags) != 0) {
        return impl_->fail(
            Status::invalid_argument,
            "device buffer flags are invalid");
    }
    if (impl_->device_buffers.size() >=
        impl_->config.device_buffer_capacity) {
        return impl_->fail(
            Status::capacity_exceeded,
            "configured device buffer capacity exceeded");
    }
    const auto backend_index = static_cast<std::size_t>(
        registration.backend.index());
    const auto& backend = impl_->device_backends[backend_index];
    if (registration.storage.size() >
        backend.capabilities.max_buffer_bytes) {
        return impl_->fail(
            Status::capacity_exceeded,
            "device buffer exceeds backend byte capacity");
    }
    std::size_t backend_buffer_count = 0;
    for (const auto& buffer : impl_->device_buffers) {
        if (buffer.backend_index == backend_index) {
            ++backend_buffer_count;
        }
        if (buffer.name == name) {
            return impl_->fail(
                Status::invalid_argument,
                "device buffer names must be unique");
        }
        if (byte_spans_overlap(
                std::as_bytes(buffer.storage),
                std::as_bytes(registration.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "registered device buffer regions must not overlap");
        }
    }
    if (backend_buffer_count >=
        backend.capabilities.max_registered_buffers) {
        return impl_->fail(
            Status::capacity_exceeded,
            "backend registered-buffer capacity exceeded");
    }

    try {
        const auto index = static_cast<std::uint32_t>(
            impl_->device_buffers.size());
        impl_->device_buffers.push_back(detail::DeviceBufferSpec{
            name,
            static_cast<std::uint32_t>(backend_index),
            registration.storage,
            registration.flags,
        });
        out_buffer = DeviceBufferHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_device_phase(
    const DevicePhaseRegistration& registration,
    PhaseHandle& out_phase) noexcept {
    out_phase = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "device phase registration is frozen");
    }
    if (registration.name.empty() ||
        !registration.callback) {
        return impl_->fail(
            Status::invalid_argument,
            "device phase name and command provider are required");
    }
    if (!impl_->valid_device_backend(registration.backend)) {
        return impl_->fail(
            Status::invalid_handle,
            "device phase references an invalid or foreign backend");
    }
    if (impl_->callbacks.size() >=
        impl_->config.callback_capacity) {
        return impl_->fail(Status::capacity_exceeded, nullptr);
    }
    const auto duplicate = std::find_if(
        impl_->callbacks.begin(),
        impl_->callbacks.end(),
        [&](const Impl::RegisteredCallback& callback) {
            return callback.name == registration.name;
        });
    if (duplicate != impl_->callbacks.end()) {
        return impl_->fail(
            Status::invalid_argument,
            "phase names must be unique");
    }

    try {
        const auto index = static_cast<std::uint32_t>(
            impl_->callbacks.size());
        impl_->callbacks.push_back(Impl::RegisteredCallback{
            std::string(registration.name),
            Impl::PhaseKind::device,
            nullptr,
            registration.callback,
            registration.user_data,
            registration.backend.index(),
        });
        out_phase = PhaseHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_resource(
    std::string_view name,
    ResourceHandle& out_resource) noexcept {
    out_resource = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "resource registration is frozen");
    }
    if (name.empty()) {
        return impl_->fail(Status::invalid_argument, "resource name is required");
    }
    if (impl_->resources.size() >= kMaxResources) {
        return impl_->fail(Status::capacity_exceeded, "resource safety limit exceeded");
    }
    const auto duplicate = std::find_if(
        impl_->resources.begin(),
        impl_->resources.end(),
        [&](const Impl::RegisteredResource& resource) {
            return resource.name == name;
        });
    if (duplicate != impl_->resources.end()) {
        return impl_->fail(Status::invalid_argument, "resource names must be unique");
    }

    try {
        const auto index = static_cast<std::uint32_t>(impl_->resources.size());
        impl_->resources.push_back(
            Impl::RegisteredResource{std::string(name)});
        out_resource = ResourceHandle{impl_->graph_owner, index};
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    impl_->clear_error();
    return Status::ok;
}

Status Runtime::register_state(
    const StateRegistration& registration) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "state registration is frozen");
    }
    if (registration.schema_version == 0 ||
        registration.storage.empty()) {
        return impl_->fail(
            Status::invalid_argument,
            "state schema version and non-empty storage are required");
    }
    if (impl_->states.size() >= impl_->config.state_capacity) {
        return impl_->fail(
            Status::capacity_exceeded,
            "configured state capacity exceeded");
    }

    std::array<char, replay_identifier_capacity> name{};
    if (!set_identifier(name, registration.name)) {
        return impl_->fail(
            Status::invalid_argument,
            "state name must be a stable replay identifier");
    }
    const auto duplicate = std::find_if(
        impl_->states.begin(),
        impl_->states.end(),
        [&](const Impl::RegisteredState& state) {
            return state.name == name;
        });
    if (duplicate != impl_->states.end()) {
        return impl_->fail(
            Status::invalid_argument,
            "state names must be unique");
    }
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(
                std::as_bytes(registration.storage),
                std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "registered state storage regions must not overlap");
        }
    }

    std::size_t payload_bytes = registration.storage.size();
    std::size_t record_bytes = 0;
    std::size_t required_bytes = 0;
    if (!detail::checked_artifact_multiply(
            impl_->states.size() + 1,
            detail::checkpoint_record_header_size,
            record_bytes)) {
        return impl_->fail(
            Status::capacity_exceeded,
            "registered state size overflows the snapshot format");
    }
    for (const auto& state : impl_->states) {
        if (!detail::checked_artifact_add(
                payload_bytes,
                state.storage.size(),
                payload_bytes)) {
            return impl_->fail(
                Status::capacity_exceeded,
                "registered state size overflows the snapshot format");
        }
    }
    if (!detail::checked_artifact_add(
            detail::checkpoint_header_size,
            record_bytes,
            required_bytes) ||
        !detail::checked_artifact_add(
            required_bytes,
            payload_bytes,
            required_bytes) ||
        required_bytes > impl_->config.snapshot_max_bytes) {
        return impl_->fail(
            Status::capacity_exceeded,
            "registered state exceeds snapshot_max_bytes");
    }

    try {
        impl_->states.push_back(Impl::RegisteredState{
            name,
            registration.schema_version,
            registration.storage,
        });
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::add_dependency(
    PhaseHandle prerequisite,
    PhaseHandle dependent) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "graph topology is frozen");
    }
    if (!impl_->valid_phase(prerequisite) ||
        !impl_->valid_phase(dependent)) {
        return impl_->fail(
            Status::invalid_handle,
            "dependency contains an invalid or foreign phase handle");
    }
    if (prerequisite == dependent) {
        return impl_->fail(
            Status::graph_cycle,
            "a phase cannot depend on itself");
    }
    const auto duplicate = std::find_if(
        impl_->dependencies.begin(),
        impl_->dependencies.end(),
        [&](const detail::GraphDependency& dependency) {
            return dependency.prerequisite == prerequisite &&
                   dependency.dependent == dependent;
        });
    if (duplicate != impl_->dependencies.end()) {
        return impl_->fail(
            Status::invalid_argument,
            "dependency is already registered");
    }

    try {
        impl_->dependencies.push_back(
            detail::GraphDependency{prerequisite, dependent});
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    impl_->clear_error();
    return Status::ok;
}

Status Runtime::declare_resource_access(
    PhaseHandle phase,
    ResourceHandle resource,
    ResourceAccess access) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "graph topology is frozen");
    }
    if (!impl_->valid_phase(phase) ||
        !impl_->valid_resource(resource)) {
        return impl_->fail(
            Status::invalid_handle,
            "resource access contains an invalid or foreign handle");
    }
    if (access != ResourceAccess::read &&
        access != ResourceAccess::write) {
        return impl_->fail(
            Status::invalid_argument,
            "resource access mode is invalid");
    }
    const auto duplicate = std::find_if(
        impl_->resource_accesses.begin(),
        impl_->resource_accesses.end(),
        [&](const detail::GraphResourceAccess& declaration) {
            return declaration.phase == phase &&
                   declaration.resource == resource;
        });
    if (duplicate != impl_->resource_accesses.end()) {
        return impl_->fail(
            Status::invalid_argument,
            "phase already declares access to this resource");
    }

    try {
        impl_->resource_accesses.push_back(
            detail::GraphResourceAccess{phase, resource, access});
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    impl_->clear_error();
    return Status::ok;
}

Status Runtime::finalize() noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::configuring) {
        return impl_->fail(Status::invalid_state, "finalize requires configuring state");
    }
    if (validate_config(impl_->config) != Status::ok ||
        impl_->callbacks.size() > impl_->config.callback_capacity ||
        impl_->states.size() > impl_->config.state_capacity ||
        impl_->device_backends.size() >
            impl_->config.device_backend_capacity ||
        impl_->device_buffers.size() >
            impl_->config.device_buffer_capacity) {
        return impl_->fail(Status::invalid_config, nullptr);
    }
    if (impl_->config.executor_policy == ExecutorPolicy::host_adapter &&
        (!impl_->host_executor_set ||
         impl_->host_executor.worker_count !=
             impl_->config.worker_count ||
         impl_->host_executor.queue_capacity !=
             impl_->config.executor_queue_capacity)) {
        return impl_->fail(
            Status::invalid_config,
            "host_adapter requires an attached adapter with matching capacities");
    }
    std::size_t backend_reported_bytes = 0;
    for (std::size_t backend_index = 0;
         backend_index < impl_->device_backends.size();
         ++backend_index) {
        const auto& backend =
            impl_->device_backends[backend_index];
        if (impl_->config.device_outstanding_capacity >
            backend.capabilities.max_in_flight) {
            return impl_->fail(
                Status::invalid_config,
                "device_outstanding_capacity exceeds a backend limit");
        }
        if (impl_->config.determinism_tier ==
                DeterminismTier::schedule_independent &&
            backend.capabilities.deterministic_mock == 0) {
            return impl_->fail(
                Status::invalid_config,
                "D1 requires every device backend to declare deterministic_mock");
        }
        std::size_t buffer_count = 0;
        for (const auto& buffer : impl_->device_buffers) {
            buffer_count +=
                buffer.backend_index == backend_index ? 1u : 0u;
        }
        if (buffer_count >
            backend.capabilities.max_registered_buffers) {
            return impl_->fail(
                Status::invalid_config,
                "registered buffers exceed a backend limit");
        }
        std::size_t total = 0;
        if (!detail::checked_add(
                backend_reported_bytes,
                static_cast<std::size_t>(
                    backend.capabilities.control_storage_bytes),
                total)) {
            return impl_->fail(
                Status::invalid_config,
                "backend-reported control storage overflows");
        }
        backend_reported_bytes = total;
    }

    std::vector<detail::ThreadInventoryEntry> thread_inventory;
    std::vector<ThreadPolicyReport> resolved_thread_policy_reports;
    CpuMemoryPolicySummary policy_summary{};
    const char* policy_error = nullptr;
    try {
        thread_inventory.reserve(
            impl_->config.worker_count + 3);
        thread_inventory.push_back({
            ThreadResourceId{ThreadRole::frame, 0},
            ThreadOwnership::host,
        });
        const auto worker_ownership =
            impl_->config.executor_policy ==
                ExecutorPolicy::host_adapter
            ? ThreadOwnership::host
            : ThreadOwnership::runtime;
        for (std::size_t index = 0;
             index < impl_->config.worker_count;
             ++index) {
            thread_inventory.push_back({
                ThreadResourceId{
                    ThreadRole::executor_worker,
                    static_cast<std::uint32_t>(index)},
                worker_ownership,
            });
        }
        if (impl_->config.watchdog_timeout_ns != 0) {
            thread_inventory.push_back({
                ThreadResourceId{ThreadRole::watchdog_service, 0},
                ThreadOwnership::runtime,
            });
        }
        if (!impl_->device_backends.empty()) {
            thread_inventory.push_back({
                ThreadResourceId{ThreadRole::device_service, 0},
                ThreadOwnership::runtime,
            });
        }
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }
    const auto thread_policy_status =
        detail::resolve_thread_policies(
            impl_->thread_policy_requests,
            thread_inventory,
            impl_->thread_policy_provider->capabilities(),
            resolved_thread_policy_reports,
            policy_error);
    if (thread_policy_status != Status::ok) {
        return impl_->fail(thread_policy_status, policy_error);
    }
    policy_summary.thread_count =
        resolved_thread_policy_reports.size();
    for (const auto& report : resolved_thread_policy_reports) {
        if (report.ownership == ThreadOwnership::runtime) {
            ++policy_summary.runtime_owned_thread_count;
        } else {
            ++policy_summary.externally_owned_thread_count;
        }
    }

    std::vector<PhaseHandle> compiled_order;
    detail::GraphCompileDiagnostic diagnostic;
    const auto compile_status = detail::compile_graph(
        impl_->graph_owner,
        impl_->callbacks.size(),
        impl_->resources.size(),
        impl_->dependencies,
        impl_->resource_accesses,
        compiled_order,
        diagnostic);
    if (compile_status != Status::ok) {
        return impl_->fail_compile(compile_status, diagnostic);
    }

    const auto minimum_graph_queue_capacity =
        impl_->callbacks.empty()
        ? std::size_t{0}
        : impl_->config.executor_policy ==
                ExecutorPolicy::host_adapter
            ? impl_->callbacks.size()
            : 1 + ((impl_->callbacks.size() - 1) /
                   impl_->config.worker_count);
    if (impl_->config.executor_queue_capacity <
        minimum_graph_queue_capacity) {
        return impl_->fail(
            Status::invalid_config,
            "executor_queue_capacity is too small for the compiled graph");
    }
    if (impl_->config.task_scratch_slots <
        impl_->callbacks.size()) {
        return impl_->fail(
            Status::invalid_config,
            "task_scratch_slots is too small for the compiled graph");
    }

    std::size_t registered_state_bytes = 0;
    for (const auto& state : impl_->states) {
        if (!detail::checked_artifact_add(
                registered_state_bytes,
                state.storage.size(),
                registered_state_bytes)) {
            return impl_->fail(
                Status::invalid_config,
                "registered state size overflows the snapshot format");
        }
    }
    std::size_t checkpoint_record_bytes = 0;
    std::size_t checkpoint_required_bytes = 0;
    if (!detail::checked_artifact_multiply(
            impl_->states.size(),
            detail::checkpoint_record_header_size,
            checkpoint_record_bytes) ||
        !detail::checked_artifact_add(
            detail::checkpoint_header_size,
            checkpoint_record_bytes,
            checkpoint_required_bytes) ||
        !detail::checked_artifact_add(
            checkpoint_required_bytes,
            registered_state_bytes,
            checkpoint_required_bytes) ||
        checkpoint_required_bytes >
            impl_->config.snapshot_max_bytes) {
        return impl_->fail(
            Status::invalid_config,
            "registered state exceeds snapshot_max_bytes");
    }

    MemoryPlan memory_plan;
    memory_plan.memory_budget_bytes =
        impl_->config.memory_budget_bytes;
    memory_plan.phase_count = impl_->callbacks.size();
    memory_plan.phase_scratch_bytes =
        impl_->config.scratch_bytes;
    memory_plan.task_scratch_bytes =
        impl_->config.task_scratch_bytes;
    memory_plan.task_scratch_slots =
        impl_->config.task_scratch_slots;
    memory_plan.trace_capacity =
        impl_->config.trace_capacity;
    memory_plan.trace_slot_bytes =
        detail::TelemetryRing::slot_size();
    memory_plan.state_count = impl_->states.size();
    memory_plan.registered_state_bytes =
        registered_state_bytes;
    memory_plan.snapshot_max_bytes =
        impl_->config.snapshot_max_bytes;
    memory_plan.replay_input_capacity =
        impl_->config.replay_input_capacity;
    memory_plan.input_log_max_bytes =
        impl_->config.input_log_max_bytes;
    memory_plan.device_backend_count =
        impl_->device_backends.size();
    memory_plan.device_buffer_count =
        impl_->device_buffers.size();
    memory_plan.device_outstanding_capacity =
        impl_->device_backends.empty()
        ? 0
        : impl_->config.device_outstanding_capacity;
    memory_plan.device_completion_batch =
        impl_->device_backends.empty()
        ? 0
        : impl_->config.device_completion_batch;
    memory_plan.device_backend_reported_bytes =
        backend_reported_bytes;
    memory_plan.scratch_alignment =
        impl_->config.scratch_alignment;
    memory_plan.overload_policy =
        impl_->config.overload_policy;

    bool plan_valid = detail::checked_align_up(
        memory_plan.phase_scratch_bytes,
        memory_plan.scratch_alignment,
        memory_plan.phase_scratch_stride);
    plan_valid = plan_valid && detail::checked_multiply(
        memory_plan.phase_count,
        memory_plan.phase_scratch_stride,
        memory_plan.phase_scratch_total_bytes);
    plan_valid = plan_valid && detail::checked_align_up(
        memory_plan.task_scratch_bytes,
        memory_plan.scratch_alignment,
        memory_plan.task_scratch_stride);
    plan_valid = plan_valid && detail::checked_multiply(
        memory_plan.task_scratch_slots,
        memory_plan.task_scratch_stride,
        memory_plan.task_scratch_total_bytes);
    plan_valid = plan_valid && detail::checked_multiply(
        memory_plan.trace_capacity,
        memory_plan.trace_slot_bytes,
        memory_plan.trace_storage_bytes);
    if (impl_->config.executor_policy ==
        ExecutorPolicy::host_adapter) {
        memory_plan.queue_slots =
            impl_->config.executor_queue_capacity;
    } else {
        plan_valid = plan_valid && detail::checked_multiply(
            impl_->config.worker_count,
            impl_->config.executor_queue_capacity,
            memory_plan.queue_slots);
    }
    plan_valid = plan_valid &&
        detail::Executor::estimate_control_storage(
            impl_->config.executor_policy,
            impl_->config.worker_count,
            impl_->config.executor_queue_capacity,
            impl_->callbacks.size(),
            impl_->dependencies.size(),
            impl_->config.task_scratch_slots,
            memory_plan.executor_control_bytes);
    if (!impl_->device_backends.empty()) {
        plan_valid = plan_valid &&
            detail::DeviceManager::estimate_control_storage(
                impl_->device_backends.size(),
                impl_->device_buffers.size(),
                impl_->config.device_outstanding_capacity,
                impl_->config.device_completion_batch,
                memory_plan.device_control_bytes);
    }

    std::size_t memory_inventory_count = 6;
    const auto add_inventory_count =
        [&](std::size_t count) {
            std::size_t total = 0;
            if (!detail::checked_add(
                    memory_inventory_count,
                    count,
                    total)) {
                return false;
            }
            memory_inventory_count = total;
            return true;
        };
    plan_valid = plan_valid &&
        add_inventory_count(resolved_thread_policy_reports.size()) &&
        add_inventory_count(impl_->device_backends.size()) &&
        add_inventory_count(impl_->states.size()) &&
        add_inventory_count(impl_->device_buffers.size());
    std::vector<MemoryRegionPolicyReport>
        resolved_memory_policy_reports;
    if (plan_valid) {
        try {
            resolved_memory_policy_reports.reserve(
                memory_inventory_count);
        } catch (const std::bad_alloc&) {
            return impl_->fail(Status::resource_exhausted, nullptr);
        } catch (...) {
            return impl_->fail(Status::internal_error, nullptr);
        }
    }

    memory_plan.runtime_control_bytes = sizeof(Impl);
    const auto add_runtime_bytes =
        [&](std::size_t bytes) {
            std::size_t total = 0;
            if (!detail::checked_add(
                    memory_plan.runtime_control_bytes,
                    bytes,
                    total)) {
                return false;
            }
            memory_plan.runtime_control_bytes = total;
            return true;
        };
    const auto add_runtime_array =
        [&](std::size_t count, std::size_t element_size) {
            std::size_t bytes = 0;
            return detail::checked_multiply(
                       count,
                       element_size,
                       bytes) &&
                   add_runtime_bytes(bytes);
        };

    plan_valid = plan_valid && add_runtime_array(
        impl_->callbacks.capacity(),
        sizeof(Impl::RegisteredCallback));
    for (const auto& callback : impl_->callbacks) {
        plan_valid = plan_valid &&
            add_runtime_bytes(callback.name.capacity() + 1);
    }
    plan_valid = plan_valid && add_runtime_array(
        impl_->device_backends.capacity(),
        sizeof(detail::DeviceBackendSpec));
    for (const auto& backend : impl_->device_backends) {
        plan_valid = plan_valid &&
            add_runtime_bytes(backend.name.capacity() + 1);
    }
    plan_valid = plan_valid && add_runtime_array(
        impl_->device_buffers.capacity(),
        sizeof(detail::DeviceBufferSpec));
    plan_valid = plan_valid && add_runtime_array(
        impl_->resources.capacity(),
        sizeof(Impl::RegisteredResource));
    for (const auto& resource : impl_->resources) {
        plan_valid = plan_valid &&
            add_runtime_bytes(resource.name.capacity() + 1);
    }
    plan_valid = plan_valid && add_runtime_array(
        impl_->states.capacity(),
        sizeof(Impl::RegisteredState));
    plan_valid = plan_valid && add_runtime_array(
        impl_->dependencies.capacity(),
        sizeof(detail::GraphDependency));
    plan_valid = plan_valid && add_runtime_array(
        impl_->resource_accesses.capacity(),
        sizeof(detail::GraphResourceAccess));
    plan_valid = plan_valid && add_runtime_array(
        compiled_order.capacity(),
        sizeof(PhaseHandle));
    plan_valid = plan_valid &&
        add_runtime_bytes(sizeof(detail::TelemetryRing));
    plan_valid = plan_valid && add_runtime_array(
        impl_->thread_policy_requests.capacity(),
        sizeof(ThreadPolicyRequest));
    plan_valid = plan_valid && add_runtime_array(
        impl_->memory_policy_requests.capacity(),
        sizeof(MemoryPolicyRequest));
    plan_valid = plan_valid && add_runtime_array(
        resolved_thread_policy_reports.capacity(),
        sizeof(ThreadPolicyReport));
    plan_valid = plan_valid && add_runtime_array(
        resolved_memory_policy_reports.capacity(),
        sizeof(MemoryRegionPolicyReport));

    std::vector<detail::MemoryInventoryEntry> memory_inventory;
    if (plan_valid) {
        try {
            memory_inventory.reserve(memory_inventory_count);
            const auto add_runtime_region =
                [&](MemoryCategory category, std::size_t bytes) {
                    memory_inventory.push_back({
                        MemoryRegionId{
                            category,
                            ThreadRole::none,
                            0},
                        MemoryProviderOwnership::runtime,
                        MemoryAccountingScope::runtime_plan,
                        bytes,
                        bytes,
                    });
                };
            add_runtime_region(
                MemoryCategory::runtime_control,
                memory_plan.runtime_control_bytes);
            add_runtime_region(
                MemoryCategory::executor_control_and_queues,
                memory_plan.executor_control_bytes);
            add_runtime_region(
                MemoryCategory::device_control_and_queues,
                memory_plan.device_control_bytes);
            add_runtime_region(
                MemoryCategory::phase_scratch,
                memory_plan.phase_scratch_total_bytes);
            add_runtime_region(
                MemoryCategory::task_scratch,
                memory_plan.task_scratch_total_bytes);
            add_runtime_region(
                MemoryCategory::trace_storage,
                memory_plan.trace_storage_bytes);
            for (const auto& thread :
                 resolved_thread_policy_reports) {
                const auto ownership =
                    thread.ownership == ThreadOwnership::runtime
                    ? MemoryProviderOwnership::runtime
                    : thread.ownership == ThreadOwnership::host
                        ? MemoryProviderOwnership::host
                        : MemoryProviderOwnership::backend;
                memory_inventory.push_back({
                    MemoryRegionId{
                        MemoryCategory::thread_stack,
                        thread.id.role,
                        thread.id.instance},
                    ownership,
                    MemoryAccountingScope::informational_excluded,
                    thread.resolved.stack_bytes,
                    0,
                });
            }
            for (std::size_t index = 0;
                 index < impl_->device_backends.size();
                 ++index) {
                memory_inventory.push_back({
                    MemoryRegionId{
                        MemoryCategory::backend_storage,
                        ThreadRole::none,
                        static_cast<std::uint32_t>(index)},
                    MemoryProviderOwnership::backend,
                    MemoryAccountingScope::informational_excluded,
                    static_cast<std::size_t>(
                        impl_->device_backends[index]
                            .capabilities.control_storage_bytes),
                    0,
                });
            }
            for (std::size_t index = 0;
                 index < impl_->states.size();
                 ++index) {
                memory_inventory.push_back({
                    MemoryRegionId{
                        MemoryCategory::registered_state,
                        ThreadRole::none,
                        static_cast<std::uint32_t>(index)},
                    MemoryProviderOwnership::host,
                    MemoryAccountingScope::informational_excluded,
                    impl_->states[index].storage.size(),
                    0,
                });
            }
            for (std::size_t index = 0;
                 index < impl_->device_buffers.size();
                 ++index) {
                memory_inventory.push_back({
                    MemoryRegionId{
                        MemoryCategory::registered_device_buffer,
                        ThreadRole::none,
                        static_cast<std::uint32_t>(index)},
                    MemoryProviderOwnership::host,
                    MemoryAccountingScope::informational_excluded,
                    impl_->device_buffers[index].storage.size(),
                    0,
                });
            }
        } catch (const std::bad_alloc&) {
            return impl_->fail(Status::resource_exhausted, nullptr);
        } catch (...) {
            return impl_->fail(Status::internal_error, nullptr);
        }
    }
    if (plan_valid &&
        memory_inventory.size() != memory_inventory_count) {
        plan_valid = false;
    }
    if (plan_valid) {
        const auto memory_policy_status =
            detail::resolve_memory_policies(
                impl_->memory_policy_requests,
                memory_inventory,
                resolved_memory_policy_reports,
                policy_summary,
                policy_error);
        if (memory_policy_status != Status::ok) {
            return impl_->fail(memory_policy_status, policy_error);
        }
    }

    memory_plan.planned_bytes =
        memory_plan.runtime_control_bytes;
    const auto add_planned_bytes =
        [&](std::size_t bytes) {
            std::size_t total = 0;
            if (!detail::checked_add(
                    memory_plan.planned_bytes,
                    bytes,
                    total)) {
                return false;
            }
            memory_plan.planned_bytes = total;
            return true;
        };
    plan_valid = plan_valid &&
        add_planned_bytes(memory_plan.executor_control_bytes) &&
        add_planned_bytes(memory_plan.device_control_bytes) &&
        add_planned_bytes(memory_plan.phase_scratch_total_bytes) &&
        add_planned_bytes(memory_plan.task_scratch_total_bytes) &&
        add_planned_bytes(memory_plan.trace_storage_bytes);
    plan_valid = plan_valid &&
        policy_summary.runtime_accounted_bytes ==
            memory_plan.planned_bytes;
    if (!plan_valid) {
        return impl_->fail(
            Status::invalid_config,
            "finalized memory plan overflows addressable storage");
    }
    if (memory_plan.planned_bytes >
        memory_plan.memory_budget_bytes) {
        return impl_->fail(
            Status::invalid_config,
            "finalized memory plan exceeds memory_budget_bytes");
    }

    std::unique_ptr<detail::Executor> executor;
    std::unique_ptr<detail::DeviceManager> devices;
    std::unique_ptr<detail::TelemetryRing> telemetry;
    detail::AlignedStorage phase_scratch;
    try {
        executor = std::make_unique<detail::Executor>(
            impl_->config.executor_policy,
            impl_->config.worker_count,
            impl_->config.executor_queue_capacity,
            impl_->callbacks.size(),
            impl_->config.task_scratch_bytes,
            impl_->config.task_scratch_slots,
            impl_->config.scratch_alignment,
            impl_->config.overload_policy,
            impl_->dependencies,
            impl_->host_executor_set
                ? &impl_->host_executor
                : nullptr);
        if (!impl_->device_backends.empty()) {
            devices = std::make_unique<detail::DeviceManager>(
                impl_->graph_owner,
                impl_->device_backends,
                impl_->device_buffers,
                impl_->config.device_outstanding_capacity,
                impl_->config.device_completion_batch);
        }
        phase_scratch.allocate(
            memory_plan.phase_scratch_total_bytes,
            memory_plan.scratch_alignment);
        telemetry =
            std::make_unique<detail::TelemetryRing>(
                impl_->config.trace_capacity);
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    impl_->numerics = NumericalPolicy(impl_->config.numerical_mode);
    impl_->compiled_order = std::move(compiled_order);
    impl_->phase_scratch = std::move(phase_scratch);
    impl_->telemetry = std::move(telemetry);
    impl_->executor = std::move(executor);
    impl_->devices = std::move(devices);
    impl_->finalized_memory_plan = memory_plan;
    impl_->thread_policy_reports =
        std::move(resolved_thread_policy_reports);
    impl_->memory_policy_reports =
        std::move(resolved_memory_policy_reports);
    impl_->cpu_memory_policy_summary = policy_summary;
    impl_->telemetry_counters.reset();
    impl_->metric_snapshot_sequence = 0;
    impl_->graph_id = impl_->compute_graph_id();
    impl_->state_schema_id =
        impl_->compute_state_schema_id();
    impl_->replay_id = impl_->compute_replay_id(
        impl_->graph_id,
        impl_->state_schema_id);
    impl_->observability = {};
    impl_->observability.config_id =
        config_identifier(impl_->config);
    impl_->observability.runtime_id =
        static_cast<std::uint64_t>(impl_->graph_owner);
    impl_->observability.trace_capacity =
        static_cast<std::uint64_t>(
            impl_->config.trace_capacity);
    std::copy_n(
        kBuildId,
        sizeof(kBuildId),
        impl_->observability.build_id.begin());
    impl_->observability.workload_id =
        impl_->config.workload_id;
    impl_->telemetry_epoch_ns = impl_->clock_now();
    impl_->state = RuntimeState::finalized;
    impl_->clear_error();
    impl_->record(
        RuntimeTraceEventType::finalized,
        Status::ok,
        impl_->telemetry_epoch_ns,
        0,
        kNoCallback,
        RuntimeTraceProducer::host,
        kNoWorker,
        impl_->observability.config_id);
    return Status::ok;
}

Status Runtime::start() noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::finalized) {
        return impl_->fail(Status::invalid_state, "start requires finalized state");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    if (!impl_->executor) {
        return impl_->fail(
            Status::internal_error,
            "finalized runtime has no executor");
    }

    auto& policy_transaction = impl_->thread_policy_transaction;
    policy_transaction.begin(
        *impl_->thread_policy_provider,
        impl_->thread_policy_reports);

    impl_->preflight_report = {};
    impl_->preflight_report.mode =
        impl_->config.platform_preflight_mode;
    impl_->preflight_report_available = true;
    if (impl_->config.platform_preflight_mode ==
        PlatformPreflightMode::strict) {
        impl_->preflight->inspect(
            impl_->finalized_memory_plan.planned_bytes,
            *impl_->clock,
            impl_->preflight_report);
        impl_->preflight_report.mode =
            PlatformPreflightMode::strict;
        if (impl_->preflight_report.check_count >
            platform_check_capacity) {
            impl_->preflight_report.check_count =
                platform_check_capacity;
        }
        bool passed =
            impl_->preflight_report.check_count ==
            platform_check_capacity;
        std::array<bool, platform_check_capacity> seen{};
        for (std::size_t index = 0;
             index < impl_->preflight_report.check_count;
             ++index) {
            auto& check = impl_->preflight_report.checks[index];
            check.message.back() = '\0';
            const auto id = static_cast<std::size_t>(check.id);
            const bool id_valid = id < seen.size();
            passed = passed && id_valid &&
                check.status == PlatformCheckStatus::passed;
            if (id_valid) {
                passed = passed && !seen[id];
                seen[id] = true;
            }
        }
        for (const bool was_seen : seen) {
            passed = passed && was_seen;
        }
        impl_->preflight_report.passed = passed;
        if (!passed) {
            return impl_->fail_preflight();
        }
    } else {
        impl_->preflight_report.passed = true;
    }

    const auto frame_policy_status =
        policy_transaction.verify_frame_thread();
    if (frame_policy_status != Status::ok) {
        policy_transaction.abort();
        return impl_->fail(
            frame_policy_status,
            "required frame-thread policy verification failed");
    }

    impl_->degradation_level.store(0, std::memory_order_release);
    if (impl_->config.watchdog_timeout_ns != 0) {
        const auto watchdog_status =
            impl_->watchdog.start(&policy_transaction);
        if (watchdog_status != Status::ok) {
            policy_transaction.abort();
            return impl_->fail(
                watchdog_status,
                "failed to start watchdog service lane");
        }
        impl_->watchdog_started = true;
        const auto policy_status = policy_transaction.failure();
        if (policy_status != Status::ok) {
            policy_transaction.abort();
            impl_->watchdog.stop();
            impl_->watchdog_started = false;
            return impl_->fail(
                policy_status,
                "required watchdog thread policy failed");
        }
    }

    const auto start_status =
        impl_->executor->start(&policy_transaction);
    if (start_status != Status::ok) {
        policy_transaction.abort();
        if (impl_->watchdog_started) {
            impl_->watchdog.stop();
            impl_->watchdog_started = false;
        }
        return impl_->fail(start_status, "failed to start executor policy");
    }
    auto policy_status = policy_transaction.failure();
    if (policy_status != Status::ok) {
        policy_transaction.abort();
        impl_->executor->stop();
        if (impl_->watchdog_started) {
            impl_->watchdog.stop();
            impl_->watchdog_started = false;
        }
        return impl_->fail(
            policy_status,
            "required executor thread policy failed");
    }
    if (impl_->devices) {
        const auto device_status = impl_->devices->start(
            *impl_->executor,
            &Impl::observe_device_event,
            impl_.get(),
            &policy_transaction);
        if (device_status != Status::ok) {
            policy_transaction.abort();
            impl_->executor->stop();
            if (impl_->watchdog_started) {
                impl_->watchdog.stop();
                impl_->watchdog_started = false;
            }
            impl_->stop_pending =
                impl_->devices->cleanup_pending();
            return impl_->fail(
                device_status,
                impl_->stop_pending
                    ? "failed to start device service lane and rollback is pending; retry stop"
                    : "failed to start device service lane");
        }
        policy_status = policy_transaction.failure();
        if (policy_status != Status::ok) {
            policy_transaction.abort();
            const auto cleanup_status = impl_->devices->stop();
            impl_->executor->stop();
            if (impl_->watchdog_started) {
                impl_->watchdog.stop();
                impl_->watchdog_started = false;
            }
            impl_->stop_pending = cleanup_status != Status::ok;
            return impl_->fail(
                policy_status,
                impl_->stop_pending
                    ? "required device thread policy failed and rollback is pending; retry stop"
                    : "required device thread policy failed");
        }
    }
    policy_transaction.commit();
    impl_->stop_pending = false;
    impl_->state = RuntimeState::running;
    impl_->clear_error();
    impl_->record(
        RuntimeTraceEventType::started,
        Status::ok,
        impl_->clock_now(),
        0);
    return Status::ok;
}

Status Runtime::step(
    const HostFrameContext& frame,
    StepResult* result) noexcept {
    StepResult local_result{};
    StepResult& output = result ? *result : local_result;
    output = {};

    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::running) {
        return impl_->fail(Status::invalid_state, "step requires non-reentrant running state");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    if (impl_->in_periodic_run.load(std::memory_order_acquire) &&
        !impl_->periodic_dispatch.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "step cannot be entered from a periodic observer");
    }
    if (impl_->in_replay.load(std::memory_order_acquire) &&
        !impl_->replay_dispatch.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "step cannot be entered from a replay input callback");
    }
    bool expected = false;
    if (!impl_->in_step.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return impl_->fail(
            Status::invalid_state,
            "step requires non-reentrant running state");
    }
    if (frame.delta.count() < 0) {
        impl_->in_step.store(false, std::memory_order_release);
        return impl_->fail(Status::invalid_argument, "frame delta cannot be negative");
    }

    struct StepGuard {
        std::atomic<bool>& flag;
        explicit StepGuard(std::atomic<bool>& value) : flag(value) {}
        ~StepGuard() { flag.store(false, std::memory_order_release); }
    } guard(impl_->in_step);

    impl_->clear_error();
    output.start_ns = impl_->clock_now();
    std::uint64_t watchdog_token = 0;
    if (impl_->config.watchdog_timeout_ns != 0) {
        std::uint64_t watchdog_deadline = 0;
        if (!checked_time_add(
                output.start_ns,
                impl_->config.watchdog_timeout_ns,
                watchdog_deadline)) {
            return impl_->fail(
                Status::clock_failure,
                "watchdog deadline overflows the runtime clock domain");
        }
        watchdog_token = impl_->watchdog.arm(
            watchdog_deadline,
            impl_->config.watchdog_timeout_ns);
        if (watchdog_token == 0) {
            return impl_->fail(
                Status::internal_error,
                "watchdog service lane is unavailable");
        }
    }
    impl_->record(
        RuntimeTraceEventType::step_begin,
        Status::ok,
        output.start_ns,
        frame.frame_index);

    impl_->active_frame = &frame;
    std::size_t failed_phase = impl_->callbacks.size();
    const auto execution_status = impl_->executor->run(
        &Impl::run_phase,
        impl_.get(),
        output.callbacks_executed,
        failed_phase);
    impl_->active_frame = nullptr;

    output.finish_ns = impl_->clock_now();
    if (watchdog_token != 0) {
        output.watchdog_fired =
            impl_->watchdog.complete(
                watchdog_token,
                output.finish_ns);
        if (output.watchdog_fired) {
            impl_->record(
                RuntimeTraceEventType::watchdog_fired,
                Status::ok,
                output.finish_ns,
                frame.frame_index,
                kNoCallback,
                RuntimeTraceProducer::host,
                kNoWorker,
                impl_->config.watchdog_timeout_ns);
            const auto current =
                impl_->degradation_level.load(
                    std::memory_order_relaxed);
            if (current <
                impl_->config.watchdog_max_degradation_level) {
                const auto next = current + 1;
                impl_->degradation_level.store(
                    next,
                    std::memory_order_release);
                impl_->record(
                    RuntimeTraceEventType::degradation_applied,
                    Status::ok,
                    output.finish_ns,
                    frame.frame_index,
                    kNoCallback,
                    RuntimeTraceProducer::host,
                    kNoWorker,
                    next);
            }
        }
    }
    output.degradation_level =
        impl_->degradation_level.load(std::memory_order_acquire);
    output.deadline_missed =
        frame.deadline_ns && output.finish_ns > *frame.deadline_ns;
    if (output.deadline_missed) {
        impl_->telemetry_counters.increment(
            RuntimeMetricId::deadline_misses);
    }
    impl_->record(
        RuntimeTraceEventType::step_end,
        execution_status,
        output.finish_ns,
        frame.frame_index);

    if (execution_status == Status::callback_failed &&
        failed_phase < impl_->callbacks.size()) {
        return impl_->fail_callback(failed_phase);
    }
    if (execution_status != Status::ok) {
        return impl_->fail(execution_status, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::run_periodic(
    const PeriodicRunConfig& config,
    PeriodicFrameObserver observer,
    void* observer_data,
    PeriodicRunResult* result) noexcept {
    PeriodicRunResult local_result{};
    PeriodicRunResult& output = result ? *result : local_result;
    output = {};

    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::running) {
        return impl_->fail(
            Status::invalid_state,
            "periodic execution requires running state");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    output.final_degradation_level =
        impl_->degradation_level.load(std::memory_order_acquire);
    if (impl_->in_step.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "periodic execution cannot be entered from a frame callback");
    }
    if (impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "periodic execution cannot run during replay");
    }
    if (config.frame_count == 0 ||
        config.period.count() <= 0 ||
        config.relative_deadline.count() <= 0) {
        return impl_->fail(
            Status::invalid_argument,
            "periodic frame_count, period, and relative deadline must be positive");
    }

    bool expected = false;
    if (!impl_->in_periodic_run.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return impl_->fail(
            Status::invalid_state,
            "periodic execution is already active");
    }
    struct PeriodicGuard {
        std::atomic<bool>& flag;
        explicit PeriodicGuard(std::atomic<bool>& value) : flag(value) {}
        ~PeriodicGuard() {
            flag.store(false, std::memory_order_release);
        }
    } guard(impl_->in_periodic_run);

    const auto period_ns =
        static_cast<std::uint64_t>(config.period.count());
    const auto relative_deadline_ns =
        static_cast<std::uint64_t>(
            config.relative_deadline.count());
    const auto first_release =
        config.first_release_ns.value_or(impl_->clock_now());
    output.first_release_ns = first_release;
    output.next_release_ns = first_release;

    const auto frames_minus_one =
        static_cast<std::uint64_t>(config.frame_count - 1);
    std::uint64_t last_offset = 0;
    std::uint64_t last_release = 0;
    std::uint64_t last_deadline = 0;
    std::uint64_t requested_next_release = 0;
    std::uint64_t last_frame_index = 0;
    if (!checked_time_multiply(
            frames_minus_one,
            period_ns,
            last_offset) ||
        !checked_time_add(
            first_release,
            last_offset,
            last_release) ||
        !checked_time_add(
            last_release,
            relative_deadline_ns,
            last_deadline) ||
        !checked_time_add(
            last_release,
            period_ns,
            requested_next_release) ||
        !checked_time_add(
            config.first_frame_index,
            frames_minus_one,
            last_frame_index)) {
        return impl_->fail(
            Status::invalid_argument,
            "periodic release, deadline, or frame index overflows");
    }
    (void)last_deadline;
    (void)last_frame_index;

    impl_->clear_error();
    std::uint64_t release = first_release;
    for (std::size_t frame_offset = 0;
         frame_offset < config.frame_count;
         ++frame_offset) {
        const auto frame_index =
            config.first_frame_index +
            static_cast<std::uint64_t>(frame_offset);
        impl_->record(
            RuntimeTraceEventType::periodic_release,
            Status::ok,
            release,
            frame_index,
            kNoCallback,
            RuntimeTraceProducer::host,
            kNoWorker,
            release);

        if (impl_->clock_sleep_until(release) != Status::ok) {
            output.final_degradation_level =
                impl_->degradation_level.load(
                    std::memory_order_acquire);
            return impl_->fail(
                Status::clock_failure,
                "runtime clock rejected an absolute periodic wait");
        }
        const auto wake = impl_->clock_now();
        impl_->record(
            RuntimeTraceEventType::periodic_wake,
            Status::ok,
            wake,
            frame_index,
            kNoCallback,
            RuntimeTraceProducer::host,
            kNoWorker,
            release);

        std::uint64_t deadline = 0;
        if (!checked_time_add(
                release,
                relative_deadline_ns,
                deadline)) {
            return impl_->fail(
                Status::invalid_argument,
                "periodic deadline overflows");
        }

        StepResult step_result;
        Status step_status = Status::internal_error;
        {
            struct PeriodicDispatchGuard {
                std::atomic<bool>& active;
                explicit PeriodicDispatchGuard(
                    std::atomic<bool>& value)
                    : active(value) {
                    active.store(true, std::memory_order_release);
                }
                ~PeriodicDispatchGuard() {
                    active.store(false, std::memory_order_release);
                }
            } dispatch_guard(impl_->periodic_dispatch);
            step_status = step(
                HostFrameContext{
                    frame_index,
                    config.period,
                    deadline,
                },
                &step_result);
        }

        PeriodicFrameResult frame_result;
        frame_result.status = step_status;
        frame_result.frame_index = frame_index;
        frame_result.release_ns = release;
        frame_result.wake_ns = wake;
        frame_result.start_ns = step_result.start_ns;
        frame_result.finish_ns = step_result.finish_ns;
        frame_result.slack_ns =
            deadline_slack(deadline, step_result.finish_ns);
        frame_result.deadline_missed =
            step_result.deadline_missed;
        frame_result.watchdog_fired =
            step_result.watchdog_fired;
        frame_result.degradation_level =
            step_result.degradation_level;

        ++output.frames_executed;
        if (frame_result.deadline_missed) {
            ++output.deadline_misses;
        }
        if (frame_result.watchdog_fired) {
            ++output.watchdog_events;
        }
        output.final_degradation_level =
            frame_result.degradation_level;
        output.last_frame = frame_result;
        output.next_release_ns =
            frame_offset + 1 == config.frame_count
            ? requested_next_release
            : release + period_ns;

        CallbackResult observer_result = CallbackResult::ok;
        if (observer) {
            try {
                observer_result = observer(
                    observer_data,
                    frame_result);
            } catch (...) {
                observer_result = CallbackResult::error;
            }
        }
        if (observer_result != CallbackResult::ok) {
            return impl_->fail(
                Status::callback_failed,
                "periodic frame observer failed");
        }
        if (step_status != Status::ok) {
            return step_status;
        }
        release += period_ns;
    }

    impl_->clear_error();
    return Status::ok;
}

Status Runtime::stop() noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "stop cannot run from a callback or periodic loop");
    }
    if (impl_->state == RuntimeState::stopped) {
        impl_->clear_error();
        return Status::ok;
    }
    if (impl_->state != RuntimeState::running &&
        impl_->state != RuntimeState::finalized) {
        return impl_->fail(Status::invalid_state, "stop requires finalized or running state");
    }

    Status device_status = Status::ok;
    if (impl_->devices) {
        device_status = impl_->devices->stop();
    }
    if (impl_->executor) {
        impl_->executor->stop();
    }
    if (impl_->watchdog_started) {
        impl_->watchdog.stop();
        impl_->watchdog_started = false;
    }
    if (device_status != Status::ok) {
        impl_->stop_pending = true;
        return impl_->fail(
            device_status,
            "device teardown failed; retry stop before releasing borrowed resources");
    }
    impl_->stop_pending = false;
    impl_->record(
        RuntimeTraceEventType::stopped,
        Status::ok,
        impl_->clock_now(),
        0);
    impl_->state = RuntimeState::stopped;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::device_health(
    DeviceBackendHandle backend,
    DeviceHealth& health) noexcept {
    health = make_device_health();
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::running ||
        !impl_->devices) {
        return impl_->fail(
            Status::invalid_state,
            "device health requires a running device service");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "device health cannot run during a frame or replay");
    }
    if (!impl_->valid_device_backend(backend)) {
        return impl_->fail(
            Status::invalid_handle,
            "device health received an invalid or foreign backend");
    }
    const auto status = impl_->devices->health(
        backend.index(),
        health);
    if (status != Status::ok) {
        return impl_->fail(status, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::reset_device(
    DeviceBackendHandle backend) noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::running ||
        !impl_->devices) {
        return impl_->fail(
            Status::invalid_state,
            "device reset requires a running device service");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "device reset cannot run during a frame or replay");
    }
    if (!impl_->valid_device_backend(backend)) {
        return impl_->fail(
            Status::invalid_handle,
            "device reset received an invalid or foreign backend");
    }
    const auto status = impl_->devices->reset(backend.index());
    if (status != Status::ok) {
        return impl_->fail(status, nullptr);
    }
    impl_->clear_error();
    return Status::ok;
}

RuntimeState Runtime::state() const noexcept {
    return impl_ ? impl_->state : RuntimeState::stopped;
}

const RuntimeConfig& Runtime::config() const noexcept {
    static const RuntimeConfig empty{};
    return impl_ ? impl_->config : empty;
}

std::size_t Runtime::callback_count() const noexcept {
    return impl_ ? impl_->callbacks.size() : 0;
}

std::size_t Runtime::device_backend_count() const noexcept {
    return impl_ ? impl_->device_backends.size() : 0;
}

std::size_t Runtime::device_buffer_count() const noexcept {
    return impl_ ? impl_->device_buffers.size() : 0;
}

std::size_t Runtime::device_phase_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    return static_cast<std::size_t>(std::count_if(
        impl_->callbacks.begin(),
        impl_->callbacks.end(),
        [](const Impl::RegisteredCallback& callback) {
            return callback.kind == Impl::PhaseKind::device;
        }));
}

std::size_t Runtime::resource_count() const noexcept {
    return impl_ ? impl_->resources.size() : 0;
}

std::size_t Runtime::dependency_count() const noexcept {
    return impl_ ? impl_->dependencies.size() : 0;
}

std::size_t Runtime::resource_access_count() const noexcept {
    return impl_ ? impl_->resource_accesses.size() : 0;
}

bool Runtime::compiled_phase_at(
    std::size_t execution_index,
    PhaseHandle& phase) const noexcept {
    phase = {};
    if (!impl_ ||
        impl_->state == RuntimeState::configuring ||
        execution_index >= impl_->compiled_order.size()) {
        return false;
    }
    phase = impl_->compiled_order[execution_index];
    return true;
}

bool Runtime::static_phase_assignment_at(
    std::size_t registration_index,
    StaticPhaseAssignment& assignment) const noexcept {
    assignment = {};
    if (!impl_ || !impl_->executor ||
        impl_->state == RuntimeState::configuring ||
        registration_index >= impl_->callbacks.size()) {
        return false;
    }

    std::size_t worker_index = 0;
    if (!impl_->executor->static_assignment(
            registration_index,
            worker_index)) {
        return false;
    }
    assignment.phase = PhaseHandle{
        impl_->graph_owner,
        static_cast<std::uint32_t>(registration_index)};
    assignment.worker_index = worker_index;
    return true;
}

ExecutorStats Runtime::executor_stats() const noexcept {
    if (!impl_ || !impl_->executor) {
        return {};
    }
    return impl_->executor->stats();
}

bool Runtime::memory_plan(MemoryPlan& plan) const noexcept {
    plan = {};
    if (!impl_ ||
        impl_->state == RuntimeState::configuring ||
        !impl_->executor) {
        return false;
    }
    plan = impl_->finalized_memory_plan;
    return true;
}

bool Runtime::cpu_memory_policy_summary(
    CpuMemoryPolicySummary& summary) const noexcept {
    summary = {};
    if (!impl_ || impl_->state == RuntimeState::configuring ||
        !impl_->executor) {
        return false;
    }
    summary = impl_->cpu_memory_policy_summary;
    return true;
}

bool Runtime::thread_policy_report_at(
    std::size_t index,
    ThreadPolicyReport& report) const noexcept {
    report = {};
    if (!impl_ || impl_->state == RuntimeState::configuring ||
        index >= impl_->thread_policy_reports.size()) {
        return false;
    }
    report = impl_->thread_policy_reports[index];
    return true;
}

bool Runtime::memory_policy_report_at(
    std::size_t index,
    MemoryRegionPolicyReport& report) const noexcept {
    report = {};
    if (!impl_ || impl_->state == RuntimeState::configuring ||
        index >= impl_->memory_policy_reports.size()) {
        return false;
    }
    report = impl_->memory_policy_reports[index];
    return true;
}

bool Runtime::platform_preflight_report(
    PlatformPreflightReport& report) const noexcept {
    report = {};
    if (!impl_ || !impl_->preflight_report_available) {
        return false;
    }
    report = impl_->preflight_report;
    return true;
}

std::uint32_t Runtime::degradation_level() const noexcept {
    return impl_
        ? impl_->degradation_level.load(std::memory_order_acquire)
        : 0;
}

std::uint64_t Runtime::now_ns() noexcept {
    return impl_ ? impl_->clock_now() : 0;
}

std::string_view Runtime::last_error() const noexcept {
    return impl_ ? std::string_view(impl_->error.data()) : std::string_view{};
}

Status Runtime::observability_metadata(
    ObservabilityMetadata& metadata) noexcept {
    metadata = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state == RuntimeState::configuring ||
        !impl_->telemetry) {
        return impl_->fail(
            Status::invalid_state,
            "observability metadata requires a finalized runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "observability export cannot run during a frame or periodic loop");
    }
    metadata = impl_->observability;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::metrics_snapshot(
    RuntimeMetricWindow window,
    RuntimeMetricCursor* cursor,
    RuntimeMetricSnapshot& snapshot) noexcept {
    snapshot = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state == RuntimeState::configuring ||
        !impl_->telemetry) {
        return impl_->fail(
            Status::invalid_state,
            "metric export requires a finalized runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "metric export cannot run during a frame or periodic loop");
    }
    if (window != RuntimeMetricWindow::cumulative &&
        window != RuntimeMetricWindow::interval) {
        return impl_->fail(
            Status::invalid_argument,
            "metric window is invalid");
    }
    if (window == RuntimeMetricWindow::interval &&
        (!cursor ||
         cursor->schema_version != observability_schema_version ||
         cursor->reserved0 != 0 ||
         (cursor->runtime_id != 0 &&
          cursor->runtime_id != impl_->observability.runtime_id))) {
        return impl_->fail(
            Status::invalid_argument,
            "interval metric cursor is invalid or belongs to another runtime");
    }
    const bool fresh_interval_cursor =
        window == RuntimeMetricWindow::interval &&
        cursor->runtime_id == 0;
    if (fresh_interval_cursor &&
        (cursor->window_end_ns != 0 ||
         !std::all_of(
             cursor->counters.begin(),
             cursor->counters.end(),
             [](std::uint64_t value) {
                 return value == 0;
             }))) {
        return impl_->fail(
            Status::invalid_argument,
            "fresh interval metric cursor is not zero initialized");
    }

    const auto current = impl_->metric_values();
    const auto end_ns = impl_->clock_now();
    if (window == RuntimeMetricWindow::interval &&
        !fresh_interval_cursor &&
        (cursor->window_end_ns < impl_->telemetry_epoch_ns ||
         cursor->window_end_ns > end_ns)) {
        return impl_->fail(
            Status::invalid_argument,
            "interval metric cursor window is outside the runtime clock range");
    }
    snapshot.metadata = impl_->observability;
    snapshot.window = window;
    snapshot.window_start_ns =
        window == RuntimeMetricWindow::interval &&
            !fresh_interval_cursor
        ? cursor->window_end_ns
        : impl_->telemetry_epoch_ns;
    snapshot.window_end_ns = end_ns;
    snapshot.sample_count = runtime_metric_count;

    for (std::size_t index = 0;
         index < runtime_metric_count;
         ++index) {
        RuntimeMetricDefinition definition;
        if (!runtime_metric_definition(index, definition)) {
            return impl_->fail(
                Status::internal_error,
                "metric schema is incomplete");
        }
        auto value = current[index];
        if (window == RuntimeMetricWindow::interval &&
            definition.kind == RuntimeMetricKind::counter) {
            if (value < cursor->counters[index]) {
                return impl_->fail(
                    Status::internal_error,
                    "monotonic metric counter regressed");
            }
            value -= cursor->counters[index];
        }
        snapshot.samples[index] = RuntimeMetricSample{
            definition.id,
            definition.kind,
            0,
            0,
            value,
        };
    }

    snapshot.snapshot_sequence =
        ++impl_->metric_snapshot_sequence;
    if (window == RuntimeMetricWindow::interval) {
        cursor->runtime_id =
            impl_->observability.runtime_id;
        cursor->window_end_ns = end_ns;
        cursor->counters = current;
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::read_trace(
    RuntimeTraceCursor& cursor,
    std::span<RuntimeTraceEvent> output,
    RuntimeTraceReadResult& result) noexcept {
    result = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state == RuntimeState::configuring ||
        !impl_->telemetry) {
        return impl_->fail(
            Status::invalid_state,
            "trace export requires a finalized runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "trace export cannot run during a frame or periodic loop");
    }
    if (cursor.schema_version != observability_schema_version ||
        cursor.reserved0 != 0 ||
        (cursor.runtime_id == 0 &&
         cursor.next_sequence != 0) ||
        (cursor.runtime_id != 0 &&
         cursor.runtime_id != impl_->observability.runtime_id)) {
        return impl_->fail(
            Status::invalid_argument,
            "trace cursor is invalid or belongs to another runtime");
    }

    const auto end = impl_->telemetry->next_sequence();
    const auto oldest =
        impl_->telemetry->oldest_sequence(end);
    std::uint64_t sequence = cursor.next_sequence;
    if (cursor.runtime_id == 0) {
        cursor.runtime_id = impl_->observability.runtime_id;
        sequence = oldest;
    } else if (sequence > end) {
        return impl_->fail(
            Status::invalid_argument,
            "trace cursor points beyond the current sequence");
    }

    result.metadata = impl_->observability;
    if (sequence < oldest) {
        result.lost_events = oldest - sequence;
        sequence = oldest;
    }
    result.first_sequence = sequence;

    while (sequence < end &&
           result.events_read < output.size()) {
        RuntimeTraceEvent event;
        if (impl_->telemetry->read_sequence(
                sequence,
                event)) {
            output[result.events_read] = event;
            ++result.events_read;
        } else {
            ++result.lost_events;
        }
        ++sequence;
    }

    cursor.next_sequence = sequence;
    result.next_sequence = sequence;
    result.remaining_sequence_count = end - sequence;
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::checkpoint_size(
    std::size_t& required_bytes) noexcept {
    required_bytes = 0;
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state == RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "checkpoint sizing requires a finalized runtime");
    }
    std::size_t record_bytes = 0;
    std::size_t payload_bytes = 0;
    if (!detail::checked_artifact_multiply(
            impl_->states.size(),
            detail::checkpoint_record_header_size,
            record_bytes)) {
        return impl_->fail(
            Status::internal_error,
            "finalized checkpoint size overflowed");
    }
    for (const auto& state : impl_->states) {
        if (!detail::checked_artifact_add(
                payload_bytes,
                state.storage.size(),
                payload_bytes)) {
            return impl_->fail(
                Status::internal_error,
                "finalized checkpoint size overflowed");
        }
    }
    if (!detail::checked_artifact_add(
            detail::checkpoint_header_size,
            record_bytes,
            required_bytes) ||
        !detail::checked_artifact_add(
            required_bytes,
            payload_bytes,
            required_bytes) ||
        required_bytes > impl_->config.snapshot_max_bytes) {
        required_bytes = 0;
        return impl_->fail(
            Status::internal_error,
            "finalized checkpoint exceeds its frozen bound");
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::write_checkpoint(
    std::uint64_t checkpoint_frame_index,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept {
    result = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state == RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "checkpoint export requires a finalized runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "checkpoint export cannot run during execution");
    }
    const auto output_bytes =
        std::span<const std::byte>(output.data(), output.size());
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(
                output_bytes,
                std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "checkpoint output cannot overlap registered state");
        }
    }

    CheckpointMetadata metadata;
    metadata.determinism_tier =
        impl_->config.determinism_tier;
    metadata.config_id = impl_->observability.config_id;
    metadata.replay_id = impl_->replay_id;
    metadata.graph_id = impl_->graph_id;
    metadata.state_schema_id = impl_->state_schema_id;
    metadata.checkpoint_frame_index =
        checkpoint_frame_index;
    metadata.build_id = impl_->observability.build_id;
    metadata.workload_id =
        impl_->observability.workload_id;
    const auto status = detail::encode_checkpoint_artifact(
        metadata,
        impl_->states.size(),
        &Impl::provide_state,
        impl_.get(),
        impl_->config.snapshot_max_bytes,
        output,
        result);
    if (status != Status::ok) {
        return impl_->fail(
            status,
            status == Status::capacity_exceeded
                ? "checkpoint output buffer is too small"
                : "checkpoint encoding failed");
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::restore_checkpoint(
    std::span<const std::byte> checkpoint,
    CheckpointMetadata* metadata) noexcept {
    if (metadata) {
        *metadata = {};
    }
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state == RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "checkpoint restore requires a finalized runtime");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "checkpoint restore cannot run during execution");
    }
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(
                checkpoint,
                std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "checkpoint input cannot overlap registered state");
        }
    }

    CheckpointMetadata parsed;
    const auto parse_status =
        detail::parse_checkpoint_artifact(
            checkpoint,
            impl_->config.snapshot_max_bytes,
            impl_->config.state_capacity,
            parsed);
    if (parse_status != Status::ok) {
        return impl_->fail(
            parse_status,
            "checkpoint validation failed before state restore");
    }
    if (parsed.runtime_version_major != version_major ||
        parsed.determinism_tier !=
            impl_->config.determinism_tier ||
        parsed.replay_id != impl_->replay_id ||
        parsed.graph_id != impl_->graph_id ||
        parsed.state_schema_id != impl_->state_schema_id ||
        parsed.state_count != impl_->states.size() ||
        parsed.workload_id != impl_->observability.workload_id ||
        (impl_->config.determinism_tier ==
             DeterminismTier::unspecified &&
         parsed.config_id != impl_->observability.config_id)) {
        return impl_->fail(
            Status::incompatible_artifact,
            "checkpoint identity does not match the finalized runtime");
    }

    // First pass verifies the exact registered schema and every destination
    // size. No application byte is changed until this pass succeeds.
    detail::CheckpointRecordCursor cursor;
    for (std::size_t index = 0;
         index < impl_->states.size();
         ++index) {
        detail::CheckpointRecordView record;
        const auto& state = impl_->states[index];
        if (!detail::next_checkpoint_record(
                checkpoint,
                parsed,
                cursor,
                record) ||
            record.name != identifier_view(state.name) ||
            record.schema_version != state.schema_version ||
            record.payload.size() != state.storage.size()) {
            return impl_->fail(
                Status::incompatible_artifact,
                "checkpoint state schema does not match registration");
        }
    }

    // The second pass cannot fail: the complete source and every destination
    // were validated above, so restore is transactional for registered bytes.
    cursor = {};
    for (std::size_t index = 0;
         index < impl_->states.size();
         ++index) {
        detail::CheckpointRecordView record;
        (void)detail::next_checkpoint_record(
            checkpoint,
            parsed,
            cursor,
            record);
        std::memcpy(
            impl_->states[index].storage.data(),
            record.payload.data(),
            record.payload.size());
    }
    if (metadata) {
        *metadata = parsed;
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::write_input_log(
    std::span<const ReplayInputRecord> records,
    std::span<std::byte> output,
    ArtifactWriteResult& result) noexcept {
    result = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state == RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "input-log export requires a finalized runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "input-log export cannot run during execution");
    }
    const auto output_bytes =
        std::span<const std::byte>(output.data(), output.size());
    if (byte_spans_overlap(
            output_bytes,
            std::as_bytes(records))) {
        return impl_->fail(
            Status::invalid_argument,
            "input-log output cannot overlap input records");
    }
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(
                output_bytes,
                std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "input-log output cannot overlap registered state");
        }
    }
    for (const auto& record : records) {
        if (byte_spans_overlap(output_bytes, record.payload)) {
            return impl_->fail(
                Status::invalid_argument,
                "input-log output cannot overlap an input payload");
        }
    }

    InputLogMetadata metadata;
    metadata.determinism_tier =
        impl_->config.determinism_tier;
    metadata.replay_id = impl_->replay_id;
    metadata.state_schema_id = impl_->state_schema_id;
    metadata.workload_id =
        impl_->observability.workload_id;
    const auto status = detail::encode_input_log_artifact(
        metadata,
        records,
        impl_->config.replay_input_capacity,
        impl_->config.input_log_max_bytes,
        output,
        result);
    if (status != Status::ok) {
        return impl_->fail(
            status,
            status == Status::capacity_exceeded
                ? "input-log output buffer is too small or exceeds its bound"
                : "input-log encoding failed");
    }
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::replay(
    std::span<const std::byte> checkpoint,
    std::span<const std::byte> input_log,
    ReplayInputCallback input_callback,
    void* input_user_data,
    ReplayResult* result) noexcept {
    ReplayResult local_result;
    ReplayResult& output = result ? *result : local_result;
    output = {};
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state != RuntimeState::running) {
        return impl_->fail(
            Status::invalid_state,
            "replay requires a running runtime");
    }
    if (impl_->stop_pending) {
        return impl_->fail(
            Status::invalid_state,
            "device teardown is pending; retry stop");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "replay requires non-reentrant host control");
    }
    for (const auto& state : impl_->states) {
        if (byte_spans_overlap(
                input_log,
                std::as_bytes(state.storage))) {
            return impl_->fail(
                Status::invalid_argument,
                "input log cannot overlap registered state");
        }
    }

    CheckpointMetadata checkpoint_metadata;
    InputLogMetadata input_metadata;
    const auto checkpoint_status =
        detail::parse_checkpoint_artifact(
            checkpoint,
            impl_->config.snapshot_max_bytes,
            impl_->config.state_capacity,
            checkpoint_metadata);
    if (checkpoint_status != Status::ok) {
        return impl_->fail(
            checkpoint_status,
            "checkpoint validation failed before replay");
    }
    const auto input_status =
        detail::parse_input_log_artifact(
            input_log,
            impl_->config.input_log_max_bytes,
            impl_->config.replay_input_capacity,
            input_metadata);
    if (input_status != Status::ok) {
        return impl_->fail(
            input_status,
            "input-log validation failed before replay");
    }
    if (checkpoint_metadata.runtime_version_major !=
            version_major ||
        checkpoint_metadata.determinism_tier !=
            impl_->config.determinism_tier ||
        checkpoint_metadata.replay_id != impl_->replay_id ||
        checkpoint_metadata.graph_id != impl_->graph_id ||
        checkpoint_metadata.state_schema_id !=
            impl_->state_schema_id ||
        checkpoint_metadata.workload_id !=
            impl_->observability.workload_id ||
        input_metadata.runtime_version_major != version_major ||
        input_metadata.determinism_tier !=
            impl_->config.determinism_tier ||
        input_metadata.replay_id != impl_->replay_id ||
        input_metadata.state_schema_id !=
            impl_->state_schema_id ||
        input_metadata.workload_id !=
            impl_->observability.workload_id ||
        (input_metadata.record_count != 0 &&
         input_metadata.first_frame_index <=
             checkpoint_metadata.checkpoint_frame_index) ||
        (impl_->config.determinism_tier ==
             DeterminismTier::unspecified &&
         checkpoint_metadata.config_id !=
             impl_->observability.config_id)) {
        return impl_->fail(
            Status::incompatible_artifact,
            "checkpoint/input-log identity does not match the runtime");
    }
    if (input_metadata.record_count != 0 && !input_callback) {
        return impl_->fail(
            Status::invalid_argument,
            "replay input callback is required for a non-empty log");
    }

    const auto restore_status =
        restore_checkpoint(checkpoint, nullptr);
    if (restore_status != Status::ok) {
        return restore_status;
    }

    bool expected = false;
    if (!impl_->in_replay.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return impl_->fail(
            Status::invalid_state,
            "replay is already active");
    }
    struct ReplayGuard {
        std::atomic<bool>& flag;
        explicit ReplayGuard(std::atomic<bool>& value)
            : flag(value) {}
        ~ReplayGuard() {
            flag.store(false, std::memory_order_release);
        }
    } replay_guard(impl_->in_replay);

    output.checkpoint_frame_index =
        checkpoint_metadata.checkpoint_frame_index;
    output.first_frame_index =
        input_metadata.first_frame_index;
    output.last_frame_index =
        input_metadata.last_frame_index;
    detail::InputLogRecordCursor cursor;
    for (std::size_t index = 0;
         index < input_metadata.record_count;
         ++index) {
        detail::InputLogRecordView record;
        if (!detail::next_input_log_record(
                input_log,
                input_metadata,
                cursor,
                record)) {
            return impl_->fail(
                Status::internal_error,
                "validated input log could not be traversed");
        }

        CallbackResult callback_result =
            CallbackResult::error;
        try {
            callback_result = input_callback(
                input_user_data,
                ReplayInputView{
                    record.frame,
                    record.input_type,
                    record.payload,
                });
        } catch (...) {
            callback_result = CallbackResult::error;
        }
        if (callback_result != CallbackResult::ok) {
            return impl_->fail(
                Status::callback_failed,
                "replay input callback failed");
        }
        ++output.records_processed;

        Status step_status = Status::internal_error;
        {
            struct ReplayDispatchGuard {
                std::atomic<bool>& flag;
                explicit ReplayDispatchGuard(
                    std::atomic<bool>& value)
                    : flag(value) {
                    flag.store(true, std::memory_order_release);
                }
                ~ReplayDispatchGuard() {
                    flag.store(false, std::memory_order_release);
                }
            } dispatch_guard(impl_->replay_dispatch);
            step_status = step(record.frame);
        }
        if (step_status != Status::ok) {
            return step_status;
        }
        ++output.frames_replayed;
    }
    output.final_state_hash = impl_->state_hash();
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::registered_state_hash(
    std::uint64_t& hash) noexcept {
    hash = 0;
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->state == RuntimeState::configuring) {
        return impl_->fail(
            Status::invalid_state,
            "registered-state hashing requires a finalized runtime");
    }
    if (impl_->in_step.load(std::memory_order_acquire) ||
        impl_->in_periodic_run.load(std::memory_order_acquire) ||
        impl_->in_replay.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "registered-state hashing cannot run during execution");
    }
    hash = impl_->state_hash();
    impl_->clear_error();
    return Status::ok;
}

std::size_t Runtime::trace_event_count() const noexcept {
    if (!impl_ || !impl_->telemetry) {
        return 0;
    }
    return impl_->telemetry->retained_count();
}

bool Runtime::trace_event(
    std::size_t chronological_index,
    RuntimeTraceEvent& event) const noexcept {
    if (!impl_ || !impl_->telemetry) {
        return false;
    }
    return impl_->telemetry->event_at(
        chronological_index,
        event);
}

} // namespace rt

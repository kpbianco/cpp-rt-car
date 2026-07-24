#include <rt/runtime.hpp>

#include "aligned_storage.hpp"
#include "compiled_graph.hpp"
#include "executor.hpp"
#include "native_platform_preflight.hpp"
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
    return rt::Status::ok;
}

} // namespace

namespace rt {

Capabilities query_capabilities() noexcept {
    // M6 adds schema-versioned, instance-local trace and metric export with
    // fixed RT-lane emission storage.
    return {true, true, true, true, true, true, true, true};
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

    struct RegisteredCallback {
        std::string name;
        FrameCallback callback = nullptr;
        void* user_data = nullptr;
    };

    struct RegisteredResource {
        std::string name;
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
        error[0] = '\0';
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
        set(
            RuntimeMetricId::degradation_level,
            degradation_level.load(std::memory_order_acquire));
        return values;
    }

    static CallbackResult run_phase(
        void* opaque,
        std::uint32_t phase_index,
        const TaskContext& task_context) {
        auto& self = *static_cast<Impl*>(opaque);
        const auto index = static_cast<std::size_t>(phase_index);
        if (index >= self.callbacks.size() || self.active_frame == nullptr) {
            return CallbackResult::error;
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
        CallbackContext callback_context{
            *self.active_frame,
            phase_scratch,
            self.numerics,
            task_context,
            self.degradation_level.load(std::memory_order_acquire),
        };

        CallbackResult result = CallbackResult::error;
        try {
            result = callback.callback(
                callback.user_data,
                callback_context);
        } catch (...) {
            result = CallbackResult::error;
        }

        self.record(
            RuntimeTraceEventType::callback_end,
            result == CallbackResult::ok
                ? Status::ok
                : Status::callback_failed,
            self.clock_now(),
            self.active_frame->frame_index,
            index,
            RuntimeTraceProducer::worker,
            task_context.worker_index(),
            task_context.task_index());
        return result;
    }

    std::uint32_t graph_owner;
    SteadyRuntimeClock owned_clock;
    RuntimeClock* clock;
    detail::NativePlatformPreflightProbe owned_preflight;
    PlatformPreflightProbe* preflight;
    RuntimeConfig config{};
    RuntimeState state = RuntimeState::configuring;
    NumericalPolicy numerics{};
    std::vector<RegisteredCallback> callbacks;
    std::vector<RegisteredResource> resources;
    std::vector<detail::GraphDependency> dependencies;
    std::vector<detail::GraphResourceAccess> resource_accesses;
    std::vector<PhaseHandle> compiled_order;
    detail::AlignedStorage phase_scratch;
    std::unique_ptr<detail::TelemetryRing> telemetry;
    detail::TelemetryCounters telemetry_counters;
    ObservabilityMetadata observability{};
    std::uint64_t telemetry_epoch_ns = 0;
    std::uint64_t metric_snapshot_sequence = 0;
    std::unique_ptr<detail::Executor> executor;
    detail::WatchdogMonitor watchdog;
    MemoryPlan finalized_memory_plan{};
    PlatformPreflightReport preflight_report{};
    bool preflight_report_available = false;
    std::atomic<bool> in_step{false};
    std::atomic<bool> in_periodic_run{false};
    std::atomic<bool> periodic_dispatch{false};
    std::atomic<std::uint32_t> degradation_level{0};
    bool watchdog_started = false;
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
        config.callback_capacity < impl_->callbacks.size()) {
        return impl_->fail(Status::invalid_config, nullptr);
    }
    impl_->config = config;
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
        candidate.callback_capacity < impl_->callbacks.size()) {
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
            registration.callback,
            registration.user_data,
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
        impl_->callbacks.size() > impl_->config.callback_capacity) {
        return impl_->fail(Status::invalid_config, nullptr);
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
    plan_valid = plan_valid && detail::checked_multiply(
        impl_->config.worker_count,
        impl_->config.executor_queue_capacity,
        memory_plan.queue_slots);
    plan_valid = plan_valid &&
        detail::Executor::estimate_control_storage(
            impl_->config.worker_count,
            impl_->config.executor_queue_capacity,
            impl_->callbacks.size(),
            impl_->dependencies.size(),
            impl_->config.task_scratch_slots,
            memory_plan.executor_control_bytes);

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
        impl_->resources.capacity(),
        sizeof(Impl::RegisteredResource));
    for (const auto& resource : impl_->resources) {
        plan_valid = plan_valid &&
            add_runtime_bytes(resource.name.capacity() + 1);
    }
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
        add_planned_bytes(memory_plan.phase_scratch_total_bytes) &&
        add_planned_bytes(memory_plan.task_scratch_total_bytes) &&
        add_planned_bytes(memory_plan.trace_storage_bytes);
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
            impl_->dependencies);
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
    impl_->finalized_memory_plan = memory_plan;
    impl_->telemetry_counters.reset();
    impl_->metric_snapshot_sequence = 0;
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
    if (!impl_->executor) {
        return impl_->fail(
            Status::internal_error,
            "finalized runtime has no executor");
    }

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

    impl_->degradation_level.store(0, std::memory_order_release);
    if (impl_->config.watchdog_timeout_ns != 0) {
        const auto watchdog_status = impl_->watchdog.start();
        if (watchdog_status != Status::ok) {
            return impl_->fail(
                watchdog_status,
                "failed to start watchdog service lane");
        }
        impl_->watchdog_started = true;
    }

    const auto start_status = impl_->executor->start();
    if (start_status != Status::ok) {
        if (impl_->watchdog_started) {
            impl_->watchdog.stop();
            impl_->watchdog_started = false;
        }
        return impl_->fail(start_status, "failed to start fixed executor team");
    }
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
    if (impl_->in_periodic_run.load(std::memory_order_acquire) &&
        !impl_->periodic_dispatch.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "step cannot be entered from a periodic observer");
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
    output.final_degradation_level =
        impl_->degradation_level.load(std::memory_order_acquire);
    if (impl_->in_step.load(std::memory_order_acquire)) {
        return impl_->fail(
            Status::invalid_state,
            "periodic execution cannot be entered from a frame callback");
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
        impl_->in_periodic_run.load(std::memory_order_acquire)) {
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

    impl_->record(
        RuntimeTraceEventType::stopped,
        Status::ok,
        impl_->clock_now(),
        0);
    if (impl_->executor) {
        impl_->executor->stop();
    }
    if (impl_->watchdog_started) {
        impl_->watchdog.stop();
        impl_->watchdog_started = false;
    }
    impl_->state = RuntimeState::stopped;
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
        impl_->in_periodic_run.load(std::memory_order_acquire)) {
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
        impl_->in_periodic_run.load(std::memory_order_acquire)) {
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
        impl_->in_periodic_run.load(std::memory_order_acquire)) {
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

#include <rt/runtime.hpp>

#include "compiled_graph.hpp"
#include "executor.hpp"

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
#include <vector>

#include <rt/arch.hpp>

namespace {

constexpr std::size_t kMaxCallbacks = 65'536;
constexpr std::size_t kMaxResources = 65'536;
constexpr std::size_t kMaxScratchBytes = std::size_t{1} << 30;
constexpr std::size_t kMaxTraceEvents = std::size_t{1} << 20;
constexpr std::size_t kMaxWorkers = 256;
constexpr std::size_t kMaxExecutorQueueCapacity = std::size_t{1} << 20;
constexpr std::size_t kNoCallback = std::numeric_limits<std::size_t>::max();

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

class SteadyRuntimeClock final : public rt::RuntimeClock {
public:
    SteadyRuntimeClock() noexcept
        : origin_(std::chrono::steady_clock::now()) {}

    std::uint64_t now_ns() noexcept override {
        const auto elapsed = std::chrono::steady_clock::now() - origin_;
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

private:
    std::chrono::steady_clock::time_point origin_;
};

bool parse_size(std::string_view value, std::size_t& parsed) noexcept {
    if (value.empty()) {
        return false;
    }

    std::uint64_t raw = 0;
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto result = std::from_chars(begin, end, raw, 10);
    if (result.ec != std::errc{} || result.ptr != end ||
        raw > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    parsed = static_cast<std::size_t>(raw);
    return true;
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
    return rt::Status::ok;
}

} // namespace

namespace rt {

Capabilities query_capabilities() noexcept {
    // M3 provides the compiled graph and unified CPU executor. The proven
    // complete bounded memory plan remains an M4 deliverable.
    return {true, true, true, false};
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
    }
    return "unknown runtime status";
}

Status set_runtime_config_value(
    RuntimeConfig& config,
    std::string_view key,
    std::string_view value) noexcept {
    RuntimeConfig candidate = config;
    std::size_t parsed = 0;

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

    explicit Impl(RuntimeClock* injected_clock)
        : graph_owner(next_graph_owner()),
          clock(injected_clock ? injected_clock : &owned_clock) {
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

    void record(
        RuntimeTraceEventType type,
        Status status,
        std::uint64_t timestamp_ns,
        std::uint64_t frame_index,
        std::size_t callback_index = kNoCallback) noexcept {
        SpinGuard guard(trace_lock);
        if (trace.empty()) {
            return;
        }
        trace[trace_write] = RuntimeTraceEvent{
            type,
            status,
            timestamp_ns,
            frame_index,
            callback_index,
        };
        trace_write = (trace_write + 1) % trace.size();
        trace_count = std::min(trace_count + 1, trace.size());
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
            index);

        std::span<std::byte> phase_scratch;
        if (self.config.scratch_bytes != 0) {
            phase_scratch = std::span<std::byte>(
                self.scratch.data() + (index * self.config.scratch_bytes),
                self.config.scratch_bytes);
        }
        CallbackContext callback_context{
            *self.active_frame,
            phase_scratch,
            self.numerics,
            task_context,
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
            index);
        return result;
    }

    std::uint32_t graph_owner;
    SteadyRuntimeClock owned_clock;
    RuntimeClock* clock;
    RuntimeConfig config{};
    RuntimeState state = RuntimeState::configuring;
    NumericalPolicy numerics{};
    std::vector<RegisteredCallback> callbacks;
    std::vector<RegisteredResource> resources;
    std::vector<detail::GraphDependency> dependencies;
    std::vector<detail::GraphResourceAccess> resource_accesses;
    std::vector<PhaseHandle> compiled_order;
    std::vector<std::byte> scratch;
    std::vector<RuntimeTraceEvent> trace;
    std::unique_ptr<detail::Executor> executor;
    std::size_t trace_write = 0;
    std::size_t trace_count = 0;
    std::atomic<bool> in_step{false};
    const HostFrameContext* active_frame = nullptr;
    std::array<char, 256> error{};
    mutable std::atomic_flag trace_lock = ATOMIC_FLAG_INIT;
    mutable std::atomic_flag error_lock = ATOMIC_FLAG_INIT;
    std::atomic_flag clock_lock = ATOMIC_FLAG_INIT;
};

Runtime::Runtime()
    : impl_(std::make_unique<Impl>(nullptr)) {}

Runtime::Runtime(RuntimeClock& clock)
    : impl_(std::make_unique<Impl>(&clock)) {}

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
    if (impl_->config.scratch_bytes != 0 &&
        impl_->callbacks.size() >
            std::numeric_limits<std::size_t>::max() /
                impl_->config.scratch_bytes) {
        return impl_->fail(
            Status::resource_exhausted,
            "phase scratch plan overflows addressable storage");
    }
    const auto scratch_plan_bytes =
        impl_->callbacks.size() * impl_->config.scratch_bytes;

    std::unique_ptr<detail::Executor> executor;
    std::vector<std::byte> scratch;
    std::vector<RuntimeTraceEvent> trace;
    try {
        executor = std::make_unique<detail::Executor>(
            impl_->config.executor_policy,
            impl_->config.worker_count,
            impl_->config.executor_queue_capacity,
            impl_->callbacks.size(),
            impl_->dependencies);
        scratch.assign(scratch_plan_bytes, std::byte{0});
        trace.assign(
            impl_->config.trace_capacity,
            RuntimeTraceEvent{});
    } catch (const std::bad_alloc&) {
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        return impl_->fail(Status::internal_error, nullptr);
    }

    impl_->numerics = NumericalPolicy(impl_->config.numerical_mode);
    impl_->compiled_order = std::move(compiled_order);
    impl_->scratch = std::move(scratch);
    impl_->trace = std::move(trace);
    impl_->executor = std::move(executor);
    impl_->trace_write = 0;
    impl_->trace_count = 0;
    impl_->state = RuntimeState::finalized;
    impl_->clear_error();
    impl_->record(
        RuntimeTraceEventType::finalized,
        Status::ok,
        impl_->clock_now(),
        0);
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
    const auto start_status = impl_->executor->start();
    if (start_status != Status::ok) {
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
    output.deadline_missed =
        frame.deadline_ns && output.finish_ns > *frame.deadline_ns;
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

Status Runtime::stop() noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->in_step.load(std::memory_order_acquire)) {
        return impl_->fail(Status::invalid_state, "stop cannot run from a callback");
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

std::uint64_t Runtime::now_ns() noexcept {
    return impl_ ? impl_->clock_now() : 0;
}

std::string_view Runtime::last_error() const noexcept {
    return impl_ ? std::string_view(impl_->error.data()) : std::string_view{};
}

std::size_t Runtime::trace_event_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    Impl::SpinGuard guard(impl_->trace_lock);
    return impl_->trace_count;
}

bool Runtime::trace_event(
    std::size_t chronological_index,
    RuntimeTraceEvent& event) const noexcept {
    if (!impl_) {
        return false;
    }
    Impl::SpinGuard guard(impl_->trace_lock);
    if (chronological_index >= impl_->trace_count || impl_->trace.empty()) {
        return false;
    }

    const std::size_t oldest = impl_->trace_count == impl_->trace.size()
        ? impl_->trace_write
        : 0;
    const std::size_t physical =
        (oldest + chronological_index) % impl_->trace.size();
    event = impl_->trace[physical];
    return true;
}

} // namespace rt

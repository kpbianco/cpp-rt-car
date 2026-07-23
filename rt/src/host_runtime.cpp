#include <rt/runtime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr std::size_t kMaxCallbacks = 65'536;
constexpr std::size_t kMaxScratchBytes = std::size_t{1} << 30;
constexpr std::size_t kMaxTraceEvents = std::size_t{1} << 20;
constexpr std::size_t kNoCallback = std::numeric_limits<std::size_t>::max();

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
    switch (config.numerical_mode) {
    case rt::NumericalMode::precise:
    case rt::NumericalMode::fused_multiply_add:
        return rt::Status::ok;
    }
    return rt::Status::invalid_config;
}

} // namespace

namespace rt {

Capabilities query_capabilities() noexcept {
    // M1 provides host-driven time. Graph compilation and the proven bounded
    // memory plan are M2 and M4 deliverables, respectively.
    return {false, true, false};
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
    struct RegisteredCallback {
        std::string name;
        FrameCallback callback = nullptr;
        void* user_data = nullptr;
    };

    explicit Impl(RuntimeClock* injected_clock)
        : clock(injected_clock ? injected_clock : &owned_clock) {
        error[0] = '\0';
    }

    void clear_error() noexcept {
        error[0] = '\0';
    }

    Status fail(Status status, const char* message) noexcept {
        if (!message) {
            message = status_message(status);
        }
        std::snprintf(error.data(), error.size(), "%s", message);
        return status;
    }

    Status fail_callback(std::size_t index) noexcept {
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

    void record(
        RuntimeTraceEventType type,
        Status status,
        std::uint64_t timestamp_ns,
        std::uint64_t frame_index,
        std::size_t callback_index = kNoCallback) noexcept {
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

    SteadyRuntimeClock owned_clock;
    RuntimeClock* clock;
    RuntimeConfig config{};
    RuntimeState state = RuntimeState::configuring;
    NumericalPolicy numerics{};
    std::vector<RegisteredCallback> callbacks;
    std::vector<std::byte> scratch;
    std::vector<RuntimeTraceEvent> trace;
    std::size_t trace_write = 0;
    std::size_t trace_count = 0;
    bool in_step = false;
    std::array<char, 256> error{};
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
        impl_->callbacks.push_back(Impl::RegisteredCallback{
            std::string(registration.name),
            registration.callback,
            registration.user_data,
        });
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

    try {
        impl_->scratch.assign(impl_->config.scratch_bytes, std::byte{0});
        impl_->trace.assign(impl_->config.trace_capacity, RuntimeTraceEvent{});
    } catch (const std::bad_alloc&) {
        impl_->scratch.clear();
        impl_->trace.clear();
        return impl_->fail(Status::resource_exhausted, nullptr);
    } catch (...) {
        impl_->scratch.clear();
        impl_->trace.clear();
        return impl_->fail(Status::internal_error, nullptr);
    }

    impl_->numerics = NumericalPolicy(impl_->config.numerical_mode);
    impl_->trace_write = 0;
    impl_->trace_count = 0;
    impl_->state = RuntimeState::finalized;
    impl_->clear_error();
    impl_->record(
        RuntimeTraceEventType::finalized,
        Status::ok,
        impl_->clock->now_ns(),
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
    impl_->state = RuntimeState::running;
    impl_->clear_error();
    impl_->record(
        RuntimeTraceEventType::started,
        Status::ok,
        impl_->clock->now_ns(),
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
    if (impl_->state != RuntimeState::running || impl_->in_step) {
        return impl_->fail(Status::invalid_state, "step requires non-reentrant running state");
    }
    if (frame.delta.count() < 0) {
        return impl_->fail(Status::invalid_argument, "frame delta cannot be negative");
    }

    struct StepGuard {
        bool& flag;
        explicit StepGuard(bool& value) : flag(value) { flag = true; }
        ~StepGuard() { flag = false; }
    } guard(impl_->in_step);

    impl_->clear_error();
    output.start_ns = impl_->clock->now_ns();
    impl_->record(
        RuntimeTraceEventType::step_begin,
        Status::ok,
        output.start_ns,
        frame.frame_index);

    CallbackContext callback_context{
        frame,
        std::span<std::byte>(impl_->scratch),
        impl_->numerics,
    };

    for (std::size_t index = 0; index < impl_->callbacks.size(); ++index) {
        auto& callback = impl_->callbacks[index];
        impl_->record(
            RuntimeTraceEventType::callback_begin,
            Status::ok,
            impl_->clock->now_ns(),
            frame.frame_index,
            index);

        CallbackResult callback_result = CallbackResult::error;
        try {
            callback_result = callback.callback(callback.user_data, callback_context);
        } catch (...) {
            callback_result = CallbackResult::error;
        }
        ++output.callbacks_executed;

        const auto callback_status = callback_result == CallbackResult::ok
            ? Status::ok
            : Status::callback_failed;
        impl_->record(
            RuntimeTraceEventType::callback_end,
            callback_status,
            impl_->clock->now_ns(),
            frame.frame_index,
            index);

        if (callback_result != CallbackResult::ok) {
            output.finish_ns = impl_->clock->now_ns();
            output.deadline_missed =
                frame.deadline_ns && output.finish_ns > *frame.deadline_ns;
            impl_->record(
                RuntimeTraceEventType::step_end,
                Status::callback_failed,
                output.finish_ns,
                frame.frame_index);
            return impl_->fail_callback(index);
        }
    }

    output.finish_ns = impl_->clock->now_ns();
    output.deadline_missed =
        frame.deadline_ns && output.finish_ns > *frame.deadline_ns;
    impl_->record(
        RuntimeTraceEventType::step_end,
        Status::ok,
        output.finish_ns,
        frame.frame_index);
    impl_->clear_error();
    return Status::ok;
}

Status Runtime::stop() noexcept {
    if (!impl_) {
        return Status::internal_error;
    }
    if (impl_->in_step) {
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
        impl_->clock->now_ns(),
        0);
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

std::uint64_t Runtime::now_ns() noexcept {
    return impl_ ? impl_->clock->now_ns() : 0;
}

std::string_view Runtime::last_error() const noexcept {
    return impl_ ? std::string_view(impl_->error.data()) : std::string_view{};
}

std::size_t Runtime::trace_event_count() const noexcept {
    return impl_ ? impl_->trace_count : 0;
}

bool Runtime::trace_event(
    std::size_t chronological_index,
    RuntimeTraceEvent& event) const noexcept {
    if (!impl_ || chronological_index >= impl_->trace_count ||
        impl_->trace.empty()) {
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

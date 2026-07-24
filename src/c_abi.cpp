#include <rt/c_api.h>
#include <rt/runtime.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <vector>

namespace {

rtfw_status to_c_status(rt::Status status) noexcept {
    switch (status) {
    case rt::Status::ok:
        return RTFW_STATUS_OK;
    case rt::Status::invalid_argument:
        return RTFW_STATUS_INVALID_ARGUMENT;
    case rt::Status::invalid_state:
        return RTFW_STATUS_INVALID_STATE;
    case rt::Status::invalid_config:
        return RTFW_STATUS_INVALID_CONFIG;
    case rt::Status::capacity_exceeded:
        return RTFW_STATUS_CAPACITY_EXCEEDED;
    case rt::Status::callback_failed:
        return RTFW_STATUS_CALLBACK_FAILED;
    case rt::Status::resource_exhausted:
        return RTFW_STATUS_RESOURCE_EXHAUSTED;
    case rt::Status::internal_error:
        return RTFW_STATUS_INTERNAL_ERROR;
    case rt::Status::invalid_handle:
        return RTFW_STATUS_INVALID_HANDLE;
    case rt::Status::graph_cycle:
        return RTFW_STATUS_GRAPH_CYCLE;
    case rt::Status::resource_conflict:
        return RTFW_STATUS_RESOURCE_CONFLICT;
    case rt::Status::queue_full:
        return RTFW_STATUS_QUEUE_FULL;
    }
    return RTFW_STATUS_INTERNAL_ERROR;
}

bool bytes_are_zero(const uint8_t* bytes, std::size_t size) noexcept {
    for (std::size_t index = 0; index < size; ++index) {
        if (bytes[index] != 0u) {
            return false;
        }
    }
    return true;
}

bool config_header_valid(const rtfw_config& config) noexcept {
    return config.struct_size >= sizeof(rtfw_config) &&
           config.abi_version == RTFW_C_ABI_VERSION &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(config.reserved),
               sizeof(config.reserved));
}

bool frame_header_valid(const rtfw_frame_context& frame) noexcept {
    return frame.struct_size >= sizeof(rtfw_frame_context) &&
           frame.reserved0 == 0u &&
           bytes_are_zero(frame.reserved1, sizeof(frame.reserved1));
}

bool result_header_valid(const rtfw_step_result& result) noexcept {
    return result.struct_size >= sizeof(rtfw_step_result) &&
           result.reserved0 == 0u &&
           bytes_are_zero(result.reserved1, sizeof(result.reserved1));
}

bool to_cpp_config(
    const rtfw_config& source,
    rt::RuntimeConfig& target) noexcept {
    if (!config_header_valid(source) ||
        source.callback_capacity > std::numeric_limits<std::size_t>::max() ||
        source.scratch_bytes > std::numeric_limits<std::size_t>::max() ||
        source.trace_capacity > std::numeric_limits<std::size_t>::max() ||
        source.worker_count > std::numeric_limits<std::size_t>::max() ||
        source.executor_queue_capacity >
            std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    target.callback_capacity =
        static_cast<std::size_t>(source.callback_capacity);
    target.scratch_bytes = static_cast<std::size_t>(source.scratch_bytes);
    target.trace_capacity = static_cast<std::size_t>(source.trace_capacity);
    switch (source.numerical_mode) {
    case RTFW_NUMERICAL_PRECISE:
        target.numerical_mode = rt::NumericalMode::precise;
        break;
    case RTFW_NUMERICAL_FUSED_MULTIPLY_ADD:
        target.numerical_mode = rt::NumericalMode::fused_multiply_add;
        break;
    default:
        return false;
    }
    switch (source.executor_policy) {
    case RTFW_EXECUTOR_STATIC_DETERMINISTIC:
        target.executor_policy =
            rt::ExecutorPolicy::static_deterministic;
        break;
    case RTFW_EXECUTOR_BOUNDED_THROUGHPUT:
        target.executor_policy =
            rt::ExecutorPolicy::bounded_throughput;
        break;
    default:
        return false;
    }
    target.worker_count =
        static_cast<std::size_t>(source.worker_count);
    target.executor_queue_capacity =
        static_cast<std::size_t>(source.executor_queue_capacity);
    return true;
}

void from_cpp_config(
    const rt::RuntimeConfig& source,
    rtfw_config& target) noexcept {
    target.callback_capacity = source.callback_capacity;
    target.scratch_bytes = source.scratch_bytes;
    target.trace_capacity = source.trace_capacity;
    target.numerical_mode =
        source.numerical_mode == rt::NumericalMode::fused_multiply_add
        ? RTFW_NUMERICAL_FUSED_MULTIPLY_ADD
        : RTFW_NUMERICAL_PRECISE;
    target.executor_policy =
        source.executor_policy ==
            rt::ExecutorPolicy::bounded_throughput
        ? RTFW_EXECUTOR_BOUNDED_THROUGHPUT
        : RTFW_EXECUTOR_STATIC_DETERMINISTIC;
    target.worker_count = source.worker_count;
    target.executor_queue_capacity =
        source.executor_queue_capacity;
}

rtfw_runtime_state to_c_state(rt::RuntimeState state) noexcept {
    switch (state) {
    case rt::RuntimeState::configuring:
        return RTFW_STATE_CONFIGURING;
    case rt::RuntimeState::finalized:
        return RTFW_STATE_FINALIZED;
    case rt::RuntimeState::running:
        return RTFW_STATE_RUNNING;
    case rt::RuntimeState::stopped:
        return RTFW_STATE_STOPPED;
    }
    return RTFW_STATE_STOPPED;
}

struct CCallback {
    rtfw_frame_callback callback = nullptr;
    void* user_data = nullptr;
};

[[nodiscard]] const rtfw_task_context* to_c_task_context(
    const rt::TaskContext& context) noexcept {
    return reinterpret_cast<const rtfw_task_context*>(&context);
}

struct CRangeInvocation {
    rtfw_range_callback callback = nullptr;
    void* user_data = nullptr;
};

rt::TaskResult invoke_c_range(
    void* opaque,
    const rt::TaskContext& context,
    const rt::TaskRange& range) {
    auto& invocation = *static_cast<CRangeInvocation*>(opaque);
    return invocation.callback(
               invocation.user_data,
               to_c_task_context(context),
               range.begin,
               range.end,
               range.task_index) == RTFW_CALLBACK_OK
        ? rt::TaskResult::ok
        : rt::TaskResult::error;
}

struct CReductionInvocation {
    rtfw_range_callback range_callback = nullptr;
    rtfw_reduction_callback combine_callback = nullptr;
    void* user_data = nullptr;
};

rt::TaskResult invoke_c_reduction_range(
    void* opaque,
    const rt::TaskContext& context,
    const rt::TaskRange& range) {
    auto& invocation = *static_cast<CReductionInvocation*>(opaque);
    return invocation.range_callback(
               invocation.user_data,
               to_c_task_context(context),
               range.begin,
               range.end,
               range.task_index) == RTFW_CALLBACK_OK
        ? rt::TaskResult::ok
        : rt::TaskResult::error;
}

rt::TaskResult invoke_c_reduction_combine(
    void* opaque,
    const rt::TaskContext& context,
    std::size_t left_task_index,
    std::size_t right_task_index) {
    auto& invocation = *static_cast<CReductionInvocation*>(opaque);
    return invocation.combine_callback(
               invocation.user_data,
               to_c_task_context(context),
               left_task_index,
               right_task_index) == RTFW_CALLBACK_OK
        ? rt::TaskResult::ok
        : rt::TaskResult::error;
}

rt::CallbackResult invoke_c_callback(
    void* opaque,
    const rt::CallbackContext& context) {
    auto* registration = static_cast<CCallback*>(opaque);
    rtfw_callback_context c_context{};
    c_context.struct_size = sizeof(c_context);
    c_context.numerical_mode =
        context.numerics.mode() == rt::NumericalMode::fused_multiply_add
        ? RTFW_NUMERICAL_FUSED_MULTIPLY_ADD
        : RTFW_NUMERICAL_PRECISE;
    c_context.frame_index = context.frame.frame_index;
    c_context.delta_ns = context.frame.delta.count();
    c_context.has_deadline = context.frame.deadline_ns ? 1u : 0u;
    c_context.deadline_ns = context.frame.deadline_ns.value_or(0);
    c_context.scratch = context.scratch.data();
    c_context.scratch_bytes = context.scratch.size();
    c_context.tasks = to_c_task_context(context.tasks);

    return registration->callback(registration->user_data, &c_context) ==
            RTFW_CALLBACK_OK
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

} // namespace

struct rtfw_handle {
    rt::Runtime runtime;
    std::vector<std::unique_ptr<CCallback>> callbacks;
    std::array<char, 256> boundary_error{};

    void clear_boundary_error() noexcept {
        boundary_error[0] = '\0';
    }

    rtfw_status fail(rtfw_status status, const char* message) noexcept {
        std::snprintf(
            boundary_error.data(),
            boundary_error.size(),
            "%s",
            message ? message : "C ABI boundary error");
        return status;
    }
};

extern "C" {

RTFW_API uint32_t rt_version_major(void) {
    return RT_VERSION_MAJOR;
}

RTFW_API uint32_t rt_version_minor(void) {
    return RT_VERSION_MINOR;
}

RTFW_API uint32_t rt_version_patch(void) {
    return RT_VERSION_PATCH;
}

RTFW_API rt_capabilities_c rt_query_capabilities(void) {
    const auto capabilities = rt::query_capabilities();
    return {
        static_cast<uint8_t>(capabilities.compiled_graph),
        static_cast<uint8_t>(capabilities.host_driven_time),
        static_cast<uint8_t>(capabilities.unified_cpu_executor),
        static_cast<uint8_t>(capabilities.bounded_memory_plan),
    };
}

RTFW_API const char* rtfw_status_message(rtfw_status status) {
    switch (status) {
    case RTFW_STATUS_OK:
        return rt::status_message(rt::Status::ok);
    case RTFW_STATUS_INVALID_ARGUMENT:
        return rt::status_message(rt::Status::invalid_argument);
    case RTFW_STATUS_INVALID_STATE:
        return rt::status_message(rt::Status::invalid_state);
    case RTFW_STATUS_INVALID_CONFIG:
        return rt::status_message(rt::Status::invalid_config);
    case RTFW_STATUS_CAPACITY_EXCEEDED:
        return rt::status_message(rt::Status::capacity_exceeded);
    case RTFW_STATUS_CALLBACK_FAILED:
        return rt::status_message(rt::Status::callback_failed);
    case RTFW_STATUS_RESOURCE_EXHAUSTED:
        return rt::status_message(rt::Status::resource_exhausted);
    case RTFW_STATUS_INTERNAL_ERROR:
        return rt::status_message(rt::Status::internal_error);
    case RTFW_STATUS_INVALID_HANDLE:
        return rt::status_message(rt::Status::invalid_handle);
    case RTFW_STATUS_GRAPH_CYCLE:
        return rt::status_message(rt::Status::graph_cycle);
    case RTFW_STATUS_RESOURCE_CONFLICT:
        return rt::status_message(rt::Status::resource_conflict);
    case RTFW_STATUS_QUEUE_FULL:
        return rt::status_message(rt::Status::queue_full);
    }
    return "unknown runtime status";
}

RTFW_API void rtfw_config_init(rtfw_config* config) {
    if (!config) {
        return;
    }
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = RTFW_C_ABI_VERSION;
    from_cpp_config(rt::RuntimeConfig{}, *config);
}

RTFW_API rtfw_status rtfw_config_set(
    rtfw_config* config,
    const char* key,
    const char* value) {
    if (!config || !key || !value || !config_header_valid(*config)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    rt::RuntimeConfig typed{};
    if (!to_cpp_config(*config, typed)) {
        return RTFW_STATUS_INVALID_CONFIG;
    }
    const auto status =
        rt::set_runtime_config_value(typed, std::string_view(key), std::string_view(value));
    if (status == rt::Status::ok) {
        from_cpp_config(typed, *config);
    }
    return to_c_status(status);
}

RTFW_API void rtfw_frame_context_init(rtfw_frame_context* context) {
    if (!context) {
        return;
    }
    std::memset(context, 0, sizeof(*context));
    context->struct_size = sizeof(*context);
}

RTFW_API void rtfw_step_result_init(rtfw_step_result* result) {
    if (!result) {
        return;
    }
    std::memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
}

RTFW_API rtfw_status rtfw_create(
    const rtfw_config* config,
    rtfw_handle** out_handle) {
    if (!out_handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;

    rt::RuntimeConfig typed{};
    if (config && !to_cpp_config(*config, typed)) {
        return RTFW_STATUS_INVALID_CONFIG;
    }

    try {
        auto handle = std::make_unique<rtfw_handle>();
        const auto status = handle->runtime.configure(typed);
        if (status != rt::Status::ok) {
            return to_c_status(status);
        }
        handle->callbacks.reserve(typed.callback_capacity);
        *out_handle = handle.release();
        return RTFW_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return RTFW_STATUS_RESOURCE_EXHAUSTED;
    } catch (...) {
        return RTFW_STATUS_INTERNAL_ERROR;
    }
}

RTFW_API rtfw_status rtfw_register_phase(
    rtfw_handle* handle,
    const char* name,
    rtfw_frame_callback callback,
    void* user_data,
    rtfw_phase_id* out_phase) {
    if (!out_phase) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_phase = RTFW_INVALID_PHASE_ID;
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    if (!name || !callback) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "callback name and function are required");
    }
    if (handle->runtime.state() != rt::RuntimeState::configuring) {
        return handle->fail(
            RTFW_STATUS_INVALID_STATE,
            "callback registration is frozen");
    }
    if (handle->callbacks.size() >=
        handle->runtime.config().callback_capacity) {
        return handle->fail(
            RTFW_STATUS_CAPACITY_EXCEEDED,
            "configured callback capacity exceeded");
    }

    handle->clear_boundary_error();
    try {
        auto registration = std::make_unique<CCallback>();
        registration->callback = callback;
        registration->user_data = user_data;
        CCallback* stable_registration = registration.get();
        handle->callbacks.push_back(std::move(registration));

        rt::PhaseHandle phase;
        const auto status = handle->runtime.register_callback(
            rt::CallbackRegistration{
                std::string_view(name),
                &invoke_c_callback,
                stable_registration,
            },
            phase);
        if (status != rt::Status::ok) {
            handle->callbacks.pop_back();
            return to_c_status(status);
        }
        *out_phase = phase.value;
        handle->clear_boundary_error();
        return RTFW_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return handle->fail(
            RTFW_STATUS_RESOURCE_EXHAUSTED,
            "callback registration allocation failed");
    } catch (...) {
        return handle->fail(
            RTFW_STATUS_INTERNAL_ERROR,
            "unexpected callback registration failure");
    }
}

RTFW_API rtfw_status rtfw_register_callback(
    rtfw_handle* handle,
    const char* name,
    rtfw_frame_callback callback,
    void* user_data) {
    rtfw_phase_id ignored = RTFW_INVALID_PHASE_ID;
    return rtfw_register_phase(
        handle,
        name,
        callback,
        user_data,
        &ignored);
}

RTFW_API rtfw_status rtfw_register_resource(
    rtfw_handle* handle,
    const char* name,
    rtfw_resource_id* out_resource) {
    if (!out_resource) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_resource = RTFW_INVALID_RESOURCE_ID;
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    if (!name) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "resource name is required");
    }

    handle->clear_boundary_error();
    rt::ResourceHandle resource;
    const auto status =
        handle->runtime.register_resource(std::string_view(name), resource);
    if (status == rt::Status::ok) {
        *out_resource = resource.value;
        handle->clear_boundary_error();
    }
    return to_c_status(status);
}

RTFW_API rtfw_status rtfw_add_dependency(
    rtfw_handle* handle,
    rtfw_phase_id prerequisite,
    rtfw_phase_id dependent) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    return to_c_status(handle->runtime.add_dependency(
        rt::PhaseHandle{prerequisite},
        rt::PhaseHandle{dependent}));
}

RTFW_API rtfw_status rtfw_declare_resource_access(
    rtfw_handle* handle,
    rtfw_phase_id phase,
    rtfw_resource_id resource,
    rtfw_resource_access access) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    rt::ResourceAccess cpp_access;
    switch (access) {
    case RTFW_RESOURCE_READ:
        cpp_access = rt::ResourceAccess::read;
        break;
    case RTFW_RESOURCE_WRITE:
        cpp_access = rt::ResourceAccess::write;
        break;
    default:
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "resource access mode is invalid");
    }

    handle->clear_boundary_error();
    return to_c_status(handle->runtime.declare_resource_access(
        rt::PhaseHandle{phase},
        rt::ResourceHandle{resource},
        cpp_access));
}

RTFW_API rtfw_status rtfw_parallel_for(
    const rtfw_task_context* context,
    uint64_t item_count,
    uint64_t grain_size,
    rtfw_range_callback callback,
    void* user_data) {
    if (!context || !callback ||
        item_count > std::numeric_limits<std::size_t>::max() ||
        grain_size > std::numeric_limits<std::size_t>::max()) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    const auto& cpp_context =
        *reinterpret_cast<const rt::TaskContext*>(context);
    CRangeInvocation invocation{callback, user_data};
    return to_c_status(cpp_context.parallel_for(
        static_cast<std::size_t>(item_count),
        static_cast<std::size_t>(grain_size),
        &invoke_c_range,
        &invocation));
}

RTFW_API rtfw_status rtfw_parallel_reduce(
    const rtfw_task_context* context,
    uint64_t item_count,
    uint64_t grain_size,
    rtfw_range_callback range_callback,
    rtfw_reduction_callback combine_callback,
    void* user_data) {
    if (!context || !range_callback || !combine_callback ||
        item_count > std::numeric_limits<std::size_t>::max() ||
        grain_size > std::numeric_limits<std::size_t>::max()) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    const auto& cpp_context =
        *reinterpret_cast<const rt::TaskContext*>(context);
    CReductionInvocation invocation{
        range_callback,
        combine_callback,
        user_data,
    };
    return to_c_status(cpp_context.parallel_reduce(
        static_cast<std::size_t>(item_count),
        static_cast<std::size_t>(grain_size),
        &invoke_c_reduction_range,
        &invoke_c_reduction_combine,
        &invocation));
}

RTFW_API rtfw_status rtfw_task_worker_index(
    const rtfw_task_context* context,
    uint64_t* out_worker_index) {
    if (!context || !out_worker_index) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    const auto& cpp_context =
        *reinterpret_cast<const rt::TaskContext*>(context);
    *out_worker_index =
        static_cast<uint64_t>(cpp_context.worker_index());
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_finalize(rtfw_handle* handle) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    return to_c_status(handle->runtime.finalize());
}

RTFW_API rtfw_status rtfw_start(rtfw_handle* handle) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    return to_c_status(handle->runtime.start());
}

RTFW_API rtfw_status rtfw_step(
    rtfw_handle* handle,
    const rtfw_frame_context* frame,
    rtfw_step_result* result) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    if (!frame || !frame_header_valid(*frame)) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "frame context is missing, undersized, or has nonzero reserved fields");
    }
    if (result && !result_header_valid(*result)) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "step result is undersized or has nonzero reserved fields");
    }
    if (frame->delta_ns < 0 || frame->has_deadline > 1u) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "invalid frame delta or deadline flag");
    }

    rt::HostFrameContext cpp_frame{};
    cpp_frame.frame_index = frame->frame_index;
    cpp_frame.delta = std::chrono::nanoseconds(frame->delta_ns);
    if (frame->has_deadline) {
        cpp_frame.deadline_ns = frame->deadline_ns;
    }

    rt::StepResult cpp_result{};
    handle->clear_boundary_error();
    const auto status = handle->runtime.step(cpp_frame, &cpp_result);
    if (result) {
        const auto struct_size = result->struct_size;
        std::memset(result, 0, sizeof(*result));
        result->struct_size = struct_size;
        result->callbacks_executed = cpp_result.callbacks_executed;
        result->start_ns = cpp_result.start_ns;
        result->finish_ns = cpp_result.finish_ns;
        result->deadline_missed = cpp_result.deadline_missed ? 1u : 0u;
    }
    return to_c_status(status);
}

RTFW_API rtfw_status rtfw_stop(rtfw_handle* handle) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    return to_c_status(handle->runtime.stop());
}

RTFW_API rtfw_status rtfw_get_state(
    const rtfw_handle* handle,
    rtfw_runtime_state* out_state) {
    if (!handle || !out_state) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_state = to_c_state(handle->runtime.state());
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_now_ns(
    rtfw_handle* handle,
    uint64_t* out_now_ns) {
    if (!handle || !out_now_ns) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_now_ns = handle->runtime.now_ns();
    return RTFW_STATUS_OK;
}

RTFW_API const char* rtfw_last_error(const rtfw_handle* handle) {
    if (!handle) {
        return "invalid runtime handle";
    }
    if (handle->boundary_error[0] != '\0') {
        return handle->boundary_error.data();
    }
    return handle->runtime.last_error().data();
}

RTFW_API void rtfw_destroy(rtfw_handle* handle) {
    delete handle;
}

} // extern "C"

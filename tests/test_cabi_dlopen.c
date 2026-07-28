#include <rt/c_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#    define RTFW_C_STATIC_ASSERT(condition, name) \
        typedef char rtfw_static_assert_##name[(condition) ? 1 : -1]
#else
#    define RTFW_C_STATIC_ASSERT(condition, name) \
        _Static_assert(condition, #name)
#endif

RTFW_C_STATIC_ASSERT(RTFW_C_ABI_VERSION == 8u, unexpected_c_abi_version);
RTFW_C_STATIC_ASSERT(
    sizeof(rtfw_status) == sizeof(int32_t),
    status_width_changed);
RTFW_C_STATIC_ASSERT(
    sizeof(rtfw_overload_policy) == sizeof(uint32_t),
    overload_policy_width_changed);
RTFW_C_STATIC_ASSERT(
    sizeof(rtfw_platform_preflight_mode) == sizeof(uint32_t),
    platform_preflight_mode_width_changed);
RTFW_C_STATIC_ASSERT(
    sizeof(rtfw_determinism_tier) == sizeof(uint32_t),
    determinism_tier_width_changed);

#undef RTFW_C_STATIC_ASSERT

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
typedef HMODULE rtfw_lib_handle;

static void lib_clear_error(void) {
    SetLastError(0);
}

static const char *lib_error(void) {
    static char buffer[256];
    DWORD err = GetLastError();
    if (err == 0) {
        return "unknown error";
    }
    DWORD len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               NULL, err, 0, buffer, (DWORD)sizeof(buffer), NULL);
    if (len == 0) {
        snprintf(buffer, sizeof(buffer), "error code %lu", (unsigned long)err);
    } else {
        while (len > 0 && (buffer[len - 1] == '\r' || buffer[len - 1] == '\n')) {
            buffer[--len] = '\0';
        }
    }
    return buffer;
}

static rtfw_lib_handle lib_open(const char *path) {
    lib_clear_error();
    return LoadLibraryA(path);
}

static void *lib_symbol(rtfw_lib_handle handle, const char *name) {
    lib_clear_error();
    return (void *)GetProcAddress(handle, name);
}

static int lib_close(rtfw_lib_handle handle) {
    if (FreeLibrary(handle)) {
        return 0;
    }
    return -1;
}
#else
#    include <dlfcn.h>
typedef void *rtfw_lib_handle;

static void lib_clear_error(void) {
    dlerror();
}

static const char *lib_error(void) {
    const char *err = dlerror();
    return err ? err : "unknown error";
}

static rtfw_lib_handle lib_open(const char *path) {
    return dlopen(path, RTLD_NOW);
}

static void *lib_symbol(rtfw_lib_handle handle, const char *name) {
    return dlsym(handle, name);
}

static int lib_close(rtfw_lib_handle handle) {
    return dlclose(handle);
}
#endif

typedef void (*rtfw_config_init_fn)(rtfw_config *);
typedef rtfw_status (*rtfw_config_set_fn)(rtfw_config *, const char *, const char *);
typedef rt_capabilities_c (*rt_query_capabilities_fn)(void);
typedef void (*rtfw_abi_info_init_fn)(rtfw_abi_info *);
typedef rtfw_status (*rtfw_get_abi_info_fn)(rtfw_abi_info *);
typedef rtfw_status (*rtfw_check_abi_fn)(uint32_t, uint64_t);
typedef void (*rtfw_host_executor_init_fn)(rtfw_host_executor *);
typedef rtfw_status (*rtfw_set_host_executor_fn)(
    rtfw_handle *, const rtfw_host_executor *);
typedef const char *(*rtfw_status_message_fn)(rtfw_status);
typedef const char *(*rtfw_metric_name_fn)(rtfw_metric_id);
typedef const char *(*rtfw_trace_event_name_fn)(rtfw_trace_event_type);
typedef void (*rtfw_frame_context_init_fn)(rtfw_frame_context *);
typedef void (*rtfw_step_result_init_fn)(rtfw_step_result *);
typedef void (*rtfw_periodic_config_init_fn)(rtfw_periodic_config *);
typedef void (*rtfw_periodic_run_result_init_fn)(rtfw_periodic_run_result *);
typedef void (*rtfw_memory_plan_init_fn)(rtfw_memory_plan *);
typedef void (*rtfw_platform_preflight_report_init_fn)(
    rtfw_platform_preflight_report *);
typedef void (*rtfw_observability_metadata_init_fn)(
    rtfw_observability_metadata *);
typedef void (*rtfw_metric_cursor_init_fn)(rtfw_metric_cursor *);
typedef void (*rtfw_metric_snapshot_init_fn)(rtfw_metric_snapshot *);
typedef void (*rtfw_trace_cursor_init_fn)(rtfw_trace_cursor *);
typedef void (*rtfw_trace_read_result_init_fn)(rtfw_trace_read_result *);
typedef void (*rtfw_artifact_write_result_init_fn)(
    rtfw_artifact_write_result *);
typedef void (*rtfw_checkpoint_metadata_init_fn)(
    rtfw_checkpoint_metadata *);
typedef void (*rtfw_input_log_metadata_init_fn)(
    rtfw_input_log_metadata *);
typedef void (*rtfw_replay_input_record_init_fn)(
    rtfw_replay_input_record *);
typedef void (*rtfw_replay_result_init_fn)(rtfw_replay_result *);
typedef void (*rtfw_device_health_init_fn)(rtfw_device_health *);
typedef rtfw_status (*rtfw_create_fn)(const rtfw_config *, rtfw_handle **);
typedef rtfw_status (*rtfw_register_callback_fn)(
    rtfw_handle *, const char *, rtfw_frame_callback, void *);
typedef rtfw_status (*rtfw_register_phase_fn)(
    rtfw_handle *, const char *, rtfw_frame_callback, void *, rtfw_phase_id *);
typedef rtfw_status (*rtfw_register_device_backend_fn)(
    rtfw_handle *,
    const char *,
    const rtfw_device_backend_api *,
    rtfw_device_backend_id *);
typedef rtfw_status (*rtfw_register_device_buffer_fn)(
    rtfw_handle *,
    const char *,
    rtfw_device_backend_id,
    void *,
    uint64_t,
    rtfw_device_buffer_flags,
    rtfw_device_buffer_id *);
typedef rtfw_status (*rtfw_register_device_phase_fn)(
    rtfw_handle *,
    const char *,
    rtfw_device_backend_id,
    rtfw_device_command_callback,
    void *,
    rtfw_phase_id *);
typedef rtfw_status (*rtfw_register_resource_fn)(
    rtfw_handle *, const char *, rtfw_resource_id *);
typedef rtfw_status (*rtfw_register_state_fn)(
    rtfw_handle *, const char *, uint32_t, void *, uint64_t);
typedef rtfw_status (*rtfw_add_dependency_fn)(
    rtfw_handle *, rtfw_phase_id, rtfw_phase_id);
typedef rtfw_status (*rtfw_declare_resource_access_fn)(
    rtfw_handle *,
    rtfw_phase_id,
    rtfw_resource_id,
    rtfw_resource_access);
typedef rtfw_status (*rtfw_parallel_for_fn)(
    const rtfw_task_context *,
    uint64_t,
    uint64_t,
    rtfw_range_callback,
    void *);
typedef rtfw_status (*rtfw_parallel_reduce_fn)(
    const rtfw_task_context *,
    uint64_t,
    uint64_t,
    rtfw_range_callback,
    rtfw_reduction_callback,
    void *);
typedef rtfw_status (*rtfw_task_worker_index_fn)(
    const rtfw_task_context *,
    uint64_t *);
typedef rtfw_status (*rtfw_task_scratch_fn)(
    const rtfw_task_context *,
    void **,
    uint64_t *);
typedef rtfw_status (*rtfw_lifecycle_fn)(rtfw_handle *);
typedef rtfw_status (*rtfw_step_fn)(
    rtfw_handle *, const rtfw_frame_context *, rtfw_step_result *);
typedef rtfw_status (*rtfw_run_periodic_fn)(
    rtfw_handle *,
    const rtfw_periodic_config *,
    rtfw_periodic_frame_callback,
    void *,
    rtfw_periodic_run_result *);
typedef rtfw_status (*rtfw_get_state_fn)(
    const rtfw_handle *, rtfw_runtime_state *);
typedef rtfw_status (*rtfw_get_memory_plan_fn)(
    const rtfw_handle *, rtfw_memory_plan *);
typedef rtfw_status (*rtfw_get_platform_preflight_report_fn)(
    const rtfw_handle *, rtfw_platform_preflight_report *);
typedef rtfw_status (*rtfw_get_degradation_level_fn)(
    const rtfw_handle *, uint32_t *);
typedef rtfw_status (*rtfw_get_device_health_fn)(
    rtfw_handle *,
    rtfw_device_backend_id,
    rtfw_device_health *);
typedef rtfw_status (*rtfw_reset_device_fn)(
    rtfw_handle *,
    rtfw_device_backend_id);
typedef rtfw_status (*rtfw_get_observability_metadata_fn)(
    rtfw_handle *, rtfw_observability_metadata *);
typedef rtfw_status (*rtfw_get_metrics_fn)(
    rtfw_handle *,
    rtfw_metric_window,
    rtfw_metric_cursor *,
    rtfw_metric_snapshot *);
typedef rtfw_status (*rtfw_read_trace_fn)(
    rtfw_handle *,
    rtfw_trace_cursor *,
    rtfw_trace_event *,
    uint64_t,
    rtfw_trace_read_result *);
typedef rtfw_status (*rtfw_checkpoint_size_fn)(
    rtfw_handle *, uint64_t *);
typedef rtfw_status (*rtfw_checkpoint_write_fn)(
    rtfw_handle *,
    uint64_t,
    void *,
    uint64_t,
    rtfw_artifact_write_result *);
typedef rtfw_status (*rtfw_checkpoint_inspect_fn)(
    const void *, uint64_t, rtfw_checkpoint_metadata *);
typedef rtfw_status (*rtfw_checkpoint_restore_fn)(
    rtfw_handle *,
    const void *,
    uint64_t,
    rtfw_checkpoint_metadata *);
typedef rtfw_status (*rtfw_input_log_write_fn)(
    rtfw_handle *,
    const rtfw_replay_input_record *,
    uint64_t,
    void *,
    uint64_t,
    rtfw_artifact_write_result *);
typedef rtfw_status (*rtfw_input_log_inspect_fn)(
    const void *, uint64_t, rtfw_input_log_metadata *);
typedef rtfw_status (*rtfw_replay_fn)(
    rtfw_handle *,
    const void *,
    uint64_t,
    const void *,
    uint64_t,
    rtfw_replay_input_callback,
    void *,
    rtfw_replay_result *);
typedef rtfw_status (*rtfw_registered_state_hash_fn)(
    rtfw_handle *, uint64_t *);
typedef const char *(*rtfw_last_error_fn)(const rtfw_handle *);
typedef void (*rtfw_destroy_fn)(rtfw_handle *);

static int check_symbol(void *sym, const char *name) {
    if (!sym) {
        fprintf(stderr, "missing symbol %s: %s\n", name, lib_error());
        return 0;
    }
    return 1;
}

#define LOAD_FUNCTION(target, symbol_name)                                      \
    do {                                                                        \
        void *raw_symbol = lib_symbol(handle, symbol_name);                     \
        if (!check_symbol(raw_symbol, symbol_name)) {                           \
            lib_close(handle);                                                  \
            return EXIT_FAILURE;                                                \
        }                                                                       \
        if (sizeof(target) != sizeof(raw_symbol)) {                             \
            fprintf(stderr, "incompatible loader pointer size for %s\n",        \
                    symbol_name);                                                \
            lib_close(handle);                                                  \
            return EXIT_FAILURE;                                                \
        }                                                                       \
        memcpy(&(target), &raw_symbol, sizeof(target));                         \
    } while (0)

typedef struct callback_state {
    uint64_t calls;
    uint64_t range_calls;
    uint64_t reduction_partials[4];
    uint64_t reduction_result;
    uint64_t combine_calls;
    uint64_t last_frame;
    uint32_t next_slot;
    uint32_t ordering_errors;
    uint32_t periodic_observations;
    uint32_t periodic_errors;
    uint64_t first_periodic_release;
} callback_state;

typedef struct callback_probe {
    callback_state *state;
    uint32_t phase;
} callback_probe;

static rtfw_parallel_for_fn loaded_parallel_for = NULL;
static rtfw_parallel_reduce_fn loaded_parallel_reduce = NULL;
static rtfw_task_worker_index_fn loaded_task_worker_index = NULL;
static rtfw_task_scratch_fn loaded_task_scratch = NULL;

static int task_context_valid(const rtfw_task_context *context) {
    uint64_t worker_index = UINT64_MAX;
    uint64_t scratch_bytes = 0;
    void *scratch = NULL;
    return context && loaded_task_worker_index && loaded_task_scratch &&
        loaded_task_worker_index(context, &worker_index) == RTFW_STATUS_OK &&
        loaded_task_scratch(context, &scratch, &scratch_bytes) ==
            RTFW_STATUS_OK &&
        worker_index != UINT64_MAX &&
        scratch != NULL &&
        scratch_bytes == 32 &&
        ((uintptr_t)scratch % 64u) == 0u;
}

static rtfw_callback_result test_range_callback(
    void *user_data,
    const rtfw_task_context *context,
    uint64_t begin,
    uint64_t end,
    uint64_t task_index) {
    callback_state *state = (callback_state *)user_data;
    (void)task_index;
    if (!state || !task_context_valid(context)) {
        return RTFW_CALLBACK_ERROR;
    }
    state->range_calls += end - begin;
    return RTFW_CALLBACK_OK;
}

static rtfw_callback_result test_reduction_range(
    void *user_data,
    const rtfw_task_context *context,
    uint64_t begin,
    uint64_t end,
    uint64_t task_index) {
    callback_state *state = (callback_state *)user_data;
    if (!state || !context || task_index >= 4 || end != begin + 1 ||
        !task_context_valid(context)) {
        return RTFW_CALLBACK_ERROR;
    }
    state->reduction_partials[task_index] = begin + 1;
    return RTFW_CALLBACK_OK;
}

static rtfw_callback_result test_reduction_combine(
    void *user_data,
    const rtfw_task_context *context,
    uint64_t left_task_index,
    uint64_t right_task_index) {
    callback_state *state = (callback_state *)user_data;
    if (!state || !context ||
        left_task_index >= 4 || right_task_index >= 4 ||
        !task_context_valid(context)) {
        return RTFW_CALLBACK_ERROR;
    }
    state->reduction_partials[left_task_index] +=
        state->reduction_partials[right_task_index];
    ++state->combine_calls;
    return RTFW_CALLBACK_OK;
}

static rtfw_callback_result test_callback(
    void *user_data,
    const rtfw_callback_context *context) {
    callback_probe *probe = (callback_probe *)user_data;
    callback_state *state = probe ? probe->state : NULL;
    if (!state || !context ||
        context->struct_size < sizeof(rtfw_callback_context) ||
        context->reserved0 != 0u ||
        context->degradation_level != 0u ||
        context->delta_ns != 1000000 ||
        context->scratch_bytes != 64 ||
        !context->scratch ||
        ((uintptr_t)context->scratch % 64u) != 0u ||
        !task_context_valid(context->tasks)) {
        return RTFW_CALLBACK_ERROR;
    }
    if (probe->phase == 1 &&
        (!loaded_parallel_for ||
         loaded_parallel_for(
             context->tasks,
             4,
             1,
             test_range_callback,
             state) != RTFW_STATUS_OK ||
         !loaded_parallel_reduce ||
         loaded_parallel_reduce(
             context->tasks,
             4,
             1,
             test_reduction_range,
             test_reduction_combine,
             state) != RTFW_STATUS_OK)) {
        return RTFW_CALLBACK_ERROR;
    }
    if (probe->phase == 1) {
        state->reduction_result = state->reduction_partials[0];
    }
    {
        const uint32_t expected_phase = state->next_slot == 0 ? 1u : 0u;
        if (probe->phase != expected_phase) {
            ++state->ordering_errors;
        }
        state->next_slot = (state->next_slot + 1u) % 2u;
    }
    ++state->calls;
    state->last_frame = context->frame_index;
    return RTFW_CALLBACK_OK;
}

static rtfw_callback_result malformed_result_callback(
    void *user_data,
    const rtfw_callback_context *context) {
    (void)user_data;
    (void)context;
    return (rtfw_callback_result)UINT32_MAX;
}

static rtfw_callback_result periodic_observer(
    void *user_data,
    const rtfw_periodic_frame_result *frame) {
    callback_state *state = (callback_state *)user_data;
    uint64_t expected_release;
    if (!state || !frame ||
        frame->struct_size < sizeof(rtfw_periodic_frame_result) ||
        frame->status != RTFW_STATUS_OK ||
        frame->frame_index !=
            100u + state->periodic_observations ||
        frame->watchdog_fired != 0u ||
        frame->degradation_level != 0u) {
        return RTFW_CALLBACK_ERROR;
    }
    if (state->periodic_observations == 0u) {
        state->first_periodic_release = frame->release_ns;
    }
    expected_release = state->first_periodic_release +
        (uint64_t)state->periodic_observations * 1000000u;
    if (frame->release_ns != expected_release) {
        ++state->periodic_errors;
    }
    ++state->periodic_observations;
    return RTFW_CALLBACK_OK;
}

static rtfw_callback_result replay_input(
    void *user_data,
    const rtfw_replay_input_view *input) {
    unsigned char *state = (unsigned char *)user_data;
    if (!state || !input ||
        input->struct_size < sizeof(*input) ||
        input->input_type != 7u ||
        input->payload_size != 8u ||
        !input->payload) {
        return RTFW_CALLBACK_ERROR;
    }
    memcpy(state, input->payload, 8u);
    return RTFW_CALLBACK_OK;
}

typedef struct cabi_device_backend {
    int initialized;
    unsigned char *buffer;
    uint64_t buffer_bytes;
    uint64_t pending_submission;
    rtfw_device_submission submission;
    uint64_t submissions;
    uint64_t completions;
    uint64_t resets;
    uint64_t unregisters;
    uint64_t shutdown_attempts;
    uint64_t shutdowns;
    uint32_t shutdown_failures_remaining;
} cabi_device_backend;

static rtfw_device_status cabi_device_capabilities(
    void *instance,
    rtfw_device_capabilities *capabilities) {
    if (!instance || !capabilities ||
        capabilities->struct_size < sizeof(*capabilities)) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->struct_size = sizeof(*capabilities);
    capabilities->abi_version = RTFW_DEVICE_ABI_VERSION;
    capabilities->max_in_flight = 64u;
    capabilities->max_registered_buffers = 64u;
    capabilities->max_buffer_bytes = 4096u;
    capabilities->inline_payload_capacity =
        RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY;
    capabilities->buffer_ref_capacity =
        RTFW_DEVICE_BUFFER_REF_CAPACITY;
    capabilities->supports_cancel = 1u;
    capabilities->supports_reset = 1u;
    capabilities->deterministic_mock = 1u;
    memcpy(
        capabilities->backend_id,
        "cabi.mock.v1",
        sizeof("cabi.mock.v1"));
    capabilities->control_storage_bytes =
        sizeof(cabi_device_backend);
    return RTFW_DEVICE_STATUS_OK;
}

static rtfw_device_status cabi_device_initialize(
    void *instance,
    const rtfw_device_init_config *config) {
    cabi_device_backend *backend = (cabi_device_backend *)instance;
    if (!backend || !config ||
        config->struct_size < sizeof(*config) ||
        config->abi_version != RTFW_DEVICE_ABI_VERSION ||
        backend->initialized) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    backend->initialized = 1;
    return RTFW_DEVICE_STATUS_OK;
}

static rtfw_device_status cabi_device_register_buffer(
    void *instance,
    const rtfw_device_buffer_registration *registration,
    uint64_t *out_token) {
    cabi_device_backend *backend = (cabi_device_backend *)instance;
    if (!backend || !backend->initialized || !registration ||
        !out_token || !registration->data ||
        registration->bytes == 0u || backend->buffer) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    backend->buffer = (unsigned char *)registration->data;
    backend->buffer_bytes = registration->bytes;
    *out_token = 1u;
    return RTFW_DEVICE_STATUS_OK;
}

static rtfw_device_status cabi_device_unregister_buffer(
    void *instance,
    uint64_t token) {
    cabi_device_backend *backend = (cabi_device_backend *)instance;
    if (!backend || !backend->initialized || token != 1u ||
        backend->pending_submission != 0u) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    ++backend->unregisters;
    backend->buffer = NULL;
    backend->buffer_bytes = 0u;
    return RTFW_DEVICE_STATUS_OK;
}

static rtfw_device_status cabi_device_submit(
    void *instance,
    const rtfw_device_submission *submission) {
    cabi_device_backend *backend = (cabi_device_backend *)instance;
    if (!backend || !backend->initialized || !submission ||
        backend->pending_submission != 0u) {
        return backend && backend->pending_submission != 0u
            ? RTFW_DEVICE_STATUS_QUEUE_FULL
            : RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    backend->submission = *submission;
    backend->pending_submission = submission->submission_id;
    ++backend->submissions;
    return RTFW_DEVICE_STATUS_OK;
}

static rtfw_device_status cabi_device_poll(
    void *instance,
    rtfw_device_completion *completions,
    uint64_t completion_capacity,
    uint64_t *out_count) {
    cabi_device_backend *backend = (cabi_device_backend *)instance;
    if (!backend || !backend->initialized || !out_count ||
        (completion_capacity != 0u && !completions)) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    *out_count = 0u;
    if (backend->pending_submission == 0u ||
        completion_capacity == 0u) {
        return RTFW_DEVICE_STATUS_OK;
    }
    if (backend->submission.opcode == 1u &&
        backend->submission.buffer_count == 1u &&
        backend->submission.payload_size == 1u &&
        backend->submission.buffers[0].buffer_token == 1u &&
        backend->submission.buffers[0].bytes <=
            backend->buffer_bytes) {
        memset(
            backend->buffer +
                backend->submission.buffers[0].offset,
            backend->submission.payload[0],
            (size_t)backend->submission.buffers[0].bytes);
    }
    memset(completions, 0, sizeof(*completions));
    completions->struct_size = sizeof(*completions);
    completions->status = RTFW_DEVICE_STATUS_OK;
    completions->submission_id = backend->pending_submission;
    completions->value = backend->submissions;
    backend->pending_submission = 0u;
    ++backend->completions;
    *out_count = 1u;
    return RTFW_DEVICE_STATUS_OK;
}

static rtfw_device_status cabi_device_cancel(
    void *instance,
    uint64_t submission_id) {
    cabi_device_backend *backend = (cabi_device_backend *)instance;
    if (!backend || !backend->initialized ||
        backend->pending_submission != submission_id) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    backend->pending_submission = 0u;
    return RTFW_DEVICE_STATUS_OK;
}

static rtfw_device_status cabi_device_health(
    void *instance,
    rtfw_device_health *health) {
    cabi_device_backend *backend = (cabi_device_backend *)instance;
    if (!backend || !backend->initialized || !health ||
        health->struct_size < sizeof(*health)) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    memset(health, 0, sizeof(*health));
    health->struct_size = sizeof(*health);
    health->state = RTFW_DEVICE_HEALTH_HEALTHY;
    health->last_status = RTFW_DEVICE_STATUS_OK;
    health->generation = backend->resets + 1u;
    health->submissions = backend->submissions;
    health->completions = backend->completions;
    health->resets = backend->resets;
    health->outstanding =
        backend->pending_submission != 0u ? 1u : 0u;
    return RTFW_DEVICE_STATUS_OK;
}

static rtfw_device_status cabi_device_reset(void *instance) {
    cabi_device_backend *backend = (cabi_device_backend *)instance;
    if (!backend || !backend->initialized ||
        backend->pending_submission != 0u) {
        return RTFW_DEVICE_STATUS_INVALID_STATE;
    }
    ++backend->resets;
    return RTFW_DEVICE_STATUS_OK;
}

static rtfw_device_status cabi_device_shutdown(void *instance) {
    cabi_device_backend *backend = (cabi_device_backend *)instance;
    if (!backend || !backend->initialized) {
        return RTFW_DEVICE_STATUS_INVALID_STATE;
    }
    ++backend->shutdown_attempts;
    if (backend->shutdown_failures_remaining != 0u) {
        --backend->shutdown_failures_remaining;
        return RTFW_DEVICE_STATUS_ERROR;
    }
    backend->initialized = 0;
    backend->pending_submission = 0u;
    ++backend->shutdowns;
    return RTFW_DEVICE_STATUS_OK;
}

typedef struct cabi_device_command {
    rtfw_device_buffer_id buffer;
    uint64_t bytes;
} cabi_device_command;

static rtfw_callback_result cabi_device_command_callback(
    void *user_data,
    const rtfw_callback_context *context,
    rtfw_device_submission *submission) {
    cabi_device_command *command = (cabi_device_command *)user_data;
    if (!command || !context || !submission ||
        submission->struct_size < sizeof(*submission) ||
        submission->abi_version != RTFW_DEVICE_ABI_VERSION) {
        return RTFW_CALLBACK_ERROR;
    }
    submission->timeout_ns = 1000000u;
    submission->opcode = 1u;
    submission->payload_size = 1u;
    submission->payload[0] = 0x6bu;
    submission->buffer_count = 1u;
    submission->buffers[0].buffer_token = command->buffer;
    submission->buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
    submission->buffers[0].bytes = command->bytes;
    return RTFW_CALLBACK_OK;
}

int main(void) {
#ifndef RTFW_LIB_PATH
#error "RTFW_LIB_PATH must be defined with the runtime library path"
#endif

    rtfw_lib_handle handle = lib_open(RTFW_LIB_PATH);
    if (!handle) {
        fprintf(stderr, "failed to load runtime: %s\n", lib_error());
        return EXIT_FAILURE;
    }

    lib_clear_error();
    rtfw_config_init_fn config_init_fn;
    rtfw_config_set_fn config_set_fn;
    rt_query_capabilities_fn capabilities_fn;
    rtfw_abi_info_init_fn abi_info_init_fn;
    rtfw_get_abi_info_fn get_abi_info_fn;
    rtfw_check_abi_fn check_abi_fn;
    rtfw_host_executor_init_fn host_executor_init_fn;
    rtfw_set_host_executor_fn set_host_executor_fn;
    rtfw_status_message_fn status_message_fn;
    rtfw_metric_name_fn metric_name_fn;
    rtfw_trace_event_name_fn trace_event_name_fn;
    rtfw_frame_context_init_fn frame_init_fn;
    rtfw_step_result_init_fn result_init_fn;
    rtfw_periodic_config_init_fn periodic_config_init_fn;
    rtfw_periodic_run_result_init_fn periodic_result_init_fn;
    rtfw_memory_plan_init_fn memory_plan_init_fn;
    rtfw_platform_preflight_report_init_fn preflight_report_init_fn;
    rtfw_observability_metadata_init_fn observability_metadata_init_fn;
    rtfw_metric_cursor_init_fn metric_cursor_init_fn;
    rtfw_metric_snapshot_init_fn metric_snapshot_init_fn;
    rtfw_trace_cursor_init_fn trace_cursor_init_fn;
    rtfw_trace_read_result_init_fn trace_read_result_init_fn;
    rtfw_artifact_write_result_init_fn artifact_result_init_fn;
    rtfw_checkpoint_metadata_init_fn checkpoint_metadata_init_fn;
    rtfw_input_log_metadata_init_fn input_log_metadata_init_fn;
    rtfw_replay_input_record_init_fn replay_record_init_fn;
    rtfw_replay_result_init_fn replay_result_init_fn;
    rtfw_device_health_init_fn device_health_init_fn;
    rtfw_create_fn create_fn;
    rtfw_register_callback_fn register_fn;
    rtfw_register_phase_fn register_phase_fn;
    rtfw_register_device_backend_fn register_device_backend_fn;
    rtfw_register_device_buffer_fn register_device_buffer_fn;
    rtfw_register_device_phase_fn register_device_phase_fn;
    rtfw_register_resource_fn register_resource_fn;
    rtfw_register_state_fn register_state_fn;
    rtfw_add_dependency_fn add_dependency_fn;
    rtfw_declare_resource_access_fn declare_access_fn;
    rtfw_parallel_for_fn parallel_for_fn;
    rtfw_parallel_reduce_fn parallel_reduce_fn;
    rtfw_task_worker_index_fn task_worker_index_fn;
    rtfw_task_scratch_fn task_scratch_fn;
    rtfw_lifecycle_fn finalize_fn;
    rtfw_lifecycle_fn start_fn;
    rtfw_step_fn step_fn;
    rtfw_run_periodic_fn run_periodic_fn;
    rtfw_lifecycle_fn stop_fn;
    rtfw_get_state_fn get_state_fn;
    rtfw_get_memory_plan_fn get_memory_plan_fn;
    rtfw_get_platform_preflight_report_fn get_preflight_report_fn;
    rtfw_get_degradation_level_fn get_degradation_level_fn;
    rtfw_get_device_health_fn get_device_health_fn;
    rtfw_reset_device_fn reset_device_fn;
    rtfw_get_observability_metadata_fn get_observability_metadata_fn;
    rtfw_get_metrics_fn get_metrics_fn;
    rtfw_read_trace_fn read_trace_fn;
    rtfw_checkpoint_size_fn checkpoint_size_fn;
    rtfw_checkpoint_write_fn checkpoint_write_fn;
    rtfw_checkpoint_inspect_fn checkpoint_inspect_fn;
    rtfw_checkpoint_restore_fn checkpoint_restore_fn;
    rtfw_input_log_write_fn input_log_write_fn;
    rtfw_input_log_inspect_fn input_log_inspect_fn;
    rtfw_replay_fn replay_fn;
    rtfw_registered_state_hash_fn registered_state_hash_fn;
    rtfw_last_error_fn last_error_fn;
    rtfw_destroy_fn destroy_fn;

    LOAD_FUNCTION(config_init_fn, "rtfw_config_init");
    LOAD_FUNCTION(config_set_fn, "rtfw_config_set");
    LOAD_FUNCTION(capabilities_fn, "rt_query_capabilities");
    LOAD_FUNCTION(abi_info_init_fn, "rtfw_abi_info_init");
    LOAD_FUNCTION(get_abi_info_fn, "rtfw_get_abi_info");
    LOAD_FUNCTION(check_abi_fn, "rtfw_check_abi");
    LOAD_FUNCTION(host_executor_init_fn, "rtfw_host_executor_init");
    LOAD_FUNCTION(set_host_executor_fn, "rtfw_set_host_executor");
    LOAD_FUNCTION(status_message_fn, "rtfw_status_message");
    LOAD_FUNCTION(metric_name_fn, "rtfw_metric_name");
    LOAD_FUNCTION(trace_event_name_fn, "rtfw_trace_event_name");
    LOAD_FUNCTION(frame_init_fn, "rtfw_frame_context_init");
    LOAD_FUNCTION(result_init_fn, "rtfw_step_result_init");
    LOAD_FUNCTION(periodic_config_init_fn, "rtfw_periodic_config_init");
    LOAD_FUNCTION(periodic_result_init_fn, "rtfw_periodic_run_result_init");
    LOAD_FUNCTION(memory_plan_init_fn, "rtfw_memory_plan_init");
    LOAD_FUNCTION(
        preflight_report_init_fn,
        "rtfw_platform_preflight_report_init");
    LOAD_FUNCTION(
        observability_metadata_init_fn,
        "rtfw_observability_metadata_init");
    LOAD_FUNCTION(metric_cursor_init_fn, "rtfw_metric_cursor_init");
    LOAD_FUNCTION(metric_snapshot_init_fn, "rtfw_metric_snapshot_init");
    LOAD_FUNCTION(trace_cursor_init_fn, "rtfw_trace_cursor_init");
    LOAD_FUNCTION(
        trace_read_result_init_fn,
        "rtfw_trace_read_result_init");
    LOAD_FUNCTION(
        artifact_result_init_fn,
        "rtfw_artifact_write_result_init");
    LOAD_FUNCTION(
        checkpoint_metadata_init_fn,
        "rtfw_checkpoint_metadata_init");
    LOAD_FUNCTION(
        input_log_metadata_init_fn,
        "rtfw_input_log_metadata_init");
    LOAD_FUNCTION(
        replay_record_init_fn,
        "rtfw_replay_input_record_init");
    LOAD_FUNCTION(
        replay_result_init_fn,
        "rtfw_replay_result_init");
    LOAD_FUNCTION(
        device_health_init_fn,
        "rtfw_device_health_init");
    LOAD_FUNCTION(create_fn, "rtfw_create");
    LOAD_FUNCTION(register_fn, "rtfw_register_callback");
    LOAD_FUNCTION(register_phase_fn, "rtfw_register_phase");
    LOAD_FUNCTION(
        register_device_backend_fn,
        "rtfw_register_device_backend");
    LOAD_FUNCTION(
        register_device_buffer_fn,
        "rtfw_register_device_buffer");
    LOAD_FUNCTION(
        register_device_phase_fn,
        "rtfw_register_device_phase");
    LOAD_FUNCTION(register_resource_fn, "rtfw_register_resource");
    LOAD_FUNCTION(register_state_fn, "rtfw_register_state");
    LOAD_FUNCTION(add_dependency_fn, "rtfw_add_dependency");
    LOAD_FUNCTION(declare_access_fn, "rtfw_declare_resource_access");
    LOAD_FUNCTION(parallel_for_fn, "rtfw_parallel_for");
    LOAD_FUNCTION(parallel_reduce_fn, "rtfw_parallel_reduce");
    LOAD_FUNCTION(task_worker_index_fn, "rtfw_task_worker_index");
    LOAD_FUNCTION(task_scratch_fn, "rtfw_task_scratch");
    LOAD_FUNCTION(finalize_fn, "rtfw_finalize");
    LOAD_FUNCTION(start_fn, "rtfw_start");
    LOAD_FUNCTION(step_fn, "rtfw_step");
    LOAD_FUNCTION(run_periodic_fn, "rtfw_run_periodic");
    LOAD_FUNCTION(stop_fn, "rtfw_stop");
    LOAD_FUNCTION(get_state_fn, "rtfw_get_state");
    LOAD_FUNCTION(get_memory_plan_fn, "rtfw_get_memory_plan");
    LOAD_FUNCTION(
        get_preflight_report_fn,
        "rtfw_get_platform_preflight_report");
    LOAD_FUNCTION(
        get_degradation_level_fn,
        "rtfw_get_degradation_level");
    LOAD_FUNCTION(
        get_device_health_fn,
        "rtfw_get_device_health");
    LOAD_FUNCTION(reset_device_fn, "rtfw_reset_device");
    LOAD_FUNCTION(
        get_observability_metadata_fn,
        "rtfw_get_observability_metadata");
    LOAD_FUNCTION(get_metrics_fn, "rtfw_get_metrics");
    LOAD_FUNCTION(read_trace_fn, "rtfw_read_trace");
    LOAD_FUNCTION(checkpoint_size_fn, "rtfw_checkpoint_size");
    LOAD_FUNCTION(checkpoint_write_fn, "rtfw_checkpoint_write");
    LOAD_FUNCTION(checkpoint_inspect_fn, "rtfw_checkpoint_inspect");
    LOAD_FUNCTION(checkpoint_restore_fn, "rtfw_checkpoint_restore");
    LOAD_FUNCTION(input_log_write_fn, "rtfw_input_log_write");
    LOAD_FUNCTION(input_log_inspect_fn, "rtfw_input_log_inspect");
    LOAD_FUNCTION(replay_fn, "rtfw_replay");
    LOAD_FUNCTION(
        registered_state_hash_fn,
        "rtfw_registered_state_hash");
    LOAD_FUNCTION(last_error_fn, "rtfw_last_error");
    LOAD_FUNCTION(destroy_fn, "rtfw_destroy");
    loaded_parallel_for = parallel_for_fn;
    loaded_parallel_reduce = parallel_reduce_fn;
    loaded_task_worker_index = task_worker_index_fn;
    loaded_task_scratch = task_scratch_fn;

    {
        rtfw_abi_info abi;
        rtfw_host_executor host_executor;
        abi_info_init_fn(&abi);
        host_executor_init_fn(&host_executor);
        if (get_abi_info_fn(&abi) != RTFW_STATUS_OK ||
            abi.abi_version != RTFW_C_ABI_VERSION ||
            abi.min_compatible_abi_version !=
                RTFW_C_ABI_MIN_COMPATIBLE_VERSION ||
            abi.layout_fingerprint !=
                RTFW_C_ABI_LAYOUT_FINGERPRINT ||
            abi.pointer_size != sizeof(void *) ||
            check_abi_fn(
                RTFW_C_ABI_VERSION,
                RTFW_C_ABI_LAYOUT_FINGERPRINT) != RTFW_STATUS_OK ||
            check_abi_fn(
                RTFW_C_ABI_VERSION - 1u,
                RTFW_C_ABI_LAYOUT_FINGERPRINT) !=
                RTFW_STATUS_INCOMPATIBLE_ABI ||
            host_executor.struct_size != sizeof(host_executor) ||
            host_executor.abi_version != RTFW_C_ABI_VERSION) {
            fprintf(stderr, "ABI handshake contract mismatch\n");
            lib_close(handle);
            return EXIT_FAILURE;
        }
        (void)set_host_executor_fn;
    }

    {
        const rt_capabilities_c capabilities = capabilities_fn();
        if (capabilities.compiled_graph != 1 ||
            capabilities.host_driven_time != 1 ||
            capabilities.unified_cpu_executor != 1 ||
            capabilities.host_executor_adapter != 1 ||
            capabilities.bounded_memory_plan != 1 ||
            capabilities.self_paced_time != 1 ||
            capabilities.frame_watchdog != 1 ||
            capabilities.strict_platform_preflight != 1 ||
            capabilities.versioned_observability != 1 ||
            capabilities.deterministic_replay != 1 ||
            capabilities.bounded_device_backend != 1 ||
            strcmp(
                metric_name_fn(RTFW_METRIC_FRAMES_COMPLETED),
                "runtime.frames_completed") != 0 ||
            strcmp(
                trace_event_name_fn(RTFW_TRACE_STEP_END),
                "frame.end") != 0 ||
            strcmp(
                metric_name_fn(RTFW_METRIC_DEVICE_COMPLETIONS),
                "device.completions") != 0 ||
            strcmp(
                trace_event_name_fn(RTFW_TRACE_DEVICE_COMPLETED),
                "device.completed") != 0 ||
            strcmp(
                status_message_fn(RTFW_STATUS_INVALID_CONFIG),
                "invalid runtime configuration") != 0 ||
            strcmp(
                status_message_fn(RTFW_STATUS_RESOURCE_CONFLICT),
                "unordered conflicting resource access") != 0 ||
            strcmp(
                status_message_fn(RTFW_STATUS_QUEUE_FULL),
                "executor queue is full") != 0 ||
            strcmp(
                status_message_fn(RTFW_STATUS_SCRATCH_EXHAUSTED),
                "task scratch plan is exhausted") != 0 ||
            strcmp(
                status_message_fn(RTFW_STATUS_DEVICE_TIMEOUT),
                "device submission timed out") != 0 ||
            strcmp(
                status_message_fn(
                    RTFW_STATUS_PLATFORM_PREFLIGHT_FAILED),
                "strict platform preflight failed") != 0 ||
            strcmp(
                status_message_fn(RTFW_STATUS_CLOCK_FAILURE),
                "runtime clock operation failed") != 0 ||
            strcmp(
                status_message_fn(RTFW_STATUS_INVALID_ARTIFACT),
                "checkpoint or input-log artifact is invalid") != 0 ||
            strcmp(
                status_message_fn(RTFW_STATUS_INCOMPATIBLE_ARTIFACT),
                "checkpoint or input-log artifact is incompatible") != 0 ||
            strcmp(
                status_message_fn(RTFW_STATUS_INCOMPATIBLE_ABI),
                "C ABI version or layout is incompatible") != 0 ||
            strcmp(
                status_message_fn((rtfw_status)INT32_MAX),
                "unknown runtime status") != 0) {
            fprintf(stderr, "capability or status contract mismatch\n");
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }

    rtfw_config config;
    config_init_fn(&config);
    if (config_set_fn(&config, "scratch_bytes", "64") != RTFW_STATUS_OK ||
        config_set_fn(&config, "callback_capacity", "2") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "executor_policy",
            "static_deterministic") != RTFW_STATUS_OK ||
        config_set_fn(&config, "worker_count", "1") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "executor_queue_capacity",
            "8") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "scratch_alignment",
            "64") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "task_scratch_bytes",
            "32") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "task_scratch_slots",
            "16") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "memory_budget_bytes",
            "1048576") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "overload_policy",
            "reject_submission") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "watchdog_timeout_ns",
            "0") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "watchdog_max_degradation_level",
            "2") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "platform_preflight_mode",
            "disabled") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "determinism_tier",
            "unspecified") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "state_capacity",
            "4") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "snapshot_max_bytes",
            "4096") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "replay_input_capacity",
            "16") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "input_log_max_bytes",
            "4096") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "workload_id",
            "cabi.dynamic") != RTFW_STATUS_OK ||
        config_set_fn(
            &config,
            "workload_id",
            "invalid workload") != RTFW_STATUS_INVALID_CONFIG ||
        config_set_fn(
            &config,
            "executor_queue_capacity",
            "3") != RTFW_STATUS_INVALID_CONFIG ||
        config_set_fn(&config, "unknown", "1") != RTFW_STATUS_INVALID_CONFIG) {
        fprintf(stderr, "strict configuration parsing failed\n");
        lib_close(handle);
        return EXIT_FAILURE;
    }
    config.reserved[0] = 1;
    if (config_set_fn(&config, "scratch_bytes", "64") !=
            RTFW_STATUS_INVALID_ARGUMENT) {
        fprintf(stderr, "configuration accepted a nonzero reserved field\n");
        lib_close(handle);
        return EXIT_FAILURE;
    }
    config.reserved[0] = 0;

    rtfw_handle *instance = NULL;
    config.numerical_mode = UINT32_MAX;
    if (create_fn(&config, &instance) != RTFW_STATUS_INVALID_CONFIG ||
        instance != NULL) {
        fprintf(stderr, "runtime accepted a malformed numerical mode\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    config.numerical_mode = RTFW_NUMERICAL_PRECISE;
    config.executor_policy = UINT32_MAX;
    if (create_fn(&config, &instance) != RTFW_STATUS_INVALID_CONFIG ||
        instance != NULL) {
        fprintf(stderr, "runtime accepted a malformed executor policy\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    config.executor_policy = RTFW_EXECUTOR_STATIC_DETERMINISTIC;
    config.overload_policy = UINT32_MAX;
    if (create_fn(&config, &instance) != RTFW_STATUS_INVALID_CONFIG ||
        instance != NULL) {
        fprintf(stderr, "runtime accepted a malformed overload policy\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    config.overload_policy = RTFW_OVERLOAD_REJECT_SUBMISSION;
    config.platform_preflight_mode = UINT32_MAX;
    if (create_fn(&config, &instance) != RTFW_STATUS_INVALID_CONFIG ||
        instance != NULL) {
        fprintf(stderr, "runtime accepted a malformed preflight mode\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    config.platform_preflight_mode = RTFW_PLATFORM_PREFLIGHT_DISABLED;
    config.determinism_tier = UINT32_MAX;
    if (create_fn(&config, &instance) != RTFW_STATUS_INVALID_CONFIG ||
        instance != NULL) {
        fprintf(stderr, "runtime accepted a malformed determinism tier\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    config.determinism_tier = RTFW_DETERMINISM_D0_UNSPECIFIED;
    if (create_fn(&config, &instance) != RTFW_STATUS_OK || !instance) {
        fprintf(stderr, "rtfw_create failed\n");
        lib_close(handle);
        return EXIT_FAILURE;
    }

    rtfw_frame_context frame;
    frame_init_fn(&frame);
    frame.delta_ns = 1000000;
    frame.reserved1[3] = 1;
    if (step_fn(instance, &frame, NULL) != RTFW_STATUS_INVALID_ARGUMENT ||
        strstr(last_error_fn(instance), "reserved") == NULL) {
        fprintf(stderr, "frame accepted a nonzero reserved field\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    frame.reserved1[3] = 0;
    if (step_fn(instance, &frame, NULL) != RTFW_STATUS_INVALID_STATE ||
        start_fn(instance) != RTFW_STATUS_INVALID_STATE) {
        fprintf(stderr, "lifecycle accepted an invalid transition\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    if (step_fn(NULL, &frame, NULL) != RTFW_STATUS_INVALID_ARGUMENT) {
        fprintf(stderr, "rtfw_step should reject NULL handles\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    {
        rtfw_memory_plan plan;
        memory_plan_init_fn(&plan);
        if (get_memory_plan_fn(instance, &plan) !=
            RTFW_STATUS_INVALID_STATE) {
            fprintf(stderr, "memory plan was visible before finalization\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }

    callback_state state = {0};
    callback_probe consumer_probe = {&state, 0};
    callback_probe producer_probe = {&state, 1};
    rtfw_phase_id consumer = RTFW_INVALID_PHASE_ID;
    rtfw_phase_id producer = RTFW_INVALID_PHASE_ID;
    rtfw_resource_id graph_state = RTFW_INVALID_RESOURCE_ID;
    if (register_phase_fn(
            instance,
            "cabi.consumer",
            test_callback,
            &consumer_probe,
            &consumer) != RTFW_STATUS_OK ||
        register_phase_fn(
            instance,
            "cabi.producer",
            test_callback,
            &producer_probe,
            &producer) != RTFW_STATUS_OK ||
        register_resource_fn(instance, "cabi.state", &graph_state) !=
            RTFW_STATUS_OK) {
        fprintf(stderr, "runtime graph registration failed\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    if (consumer == RTFW_INVALID_PHASE_ID ||
        producer == RTFW_INVALID_PHASE_ID ||
        graph_state == RTFW_INVALID_RESOURCE_ID ||
        add_dependency_fn(
            instance,
            RTFW_INVALID_PHASE_ID,
            consumer) != RTFW_STATUS_INVALID_HANDLE ||
        add_dependency_fn(
            instance,
            (rtfw_phase_id)graph_state,
            consumer) != RTFW_STATUS_INVALID_HANDLE ||
        add_dependency_fn(instance, producer, consumer) != RTFW_STATUS_OK ||
        declare_access_fn(
            instance,
            producer,
            graph_state,
            (rtfw_resource_access)99) != RTFW_STATUS_INVALID_ARGUMENT ||
        declare_access_fn(
            instance,
            producer,
            (rtfw_resource_id)producer,
            RTFW_RESOURCE_WRITE) != RTFW_STATUS_INVALID_HANDLE ||
        declare_access_fn(
            instance,
            producer,
            graph_state,
            RTFW_RESOURCE_WRITE) != RTFW_STATUS_OK ||
        declare_access_fn(
            instance,
            consumer,
            graph_state,
            RTFW_RESOURCE_READ) != RTFW_STATUS_OK) {
        fprintf(stderr, "runtime graph declaration failed\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    if (register_fn(
            instance,
            "cabi.over-capacity",
            test_callback,
            &consumer_probe) !=
            RTFW_STATUS_CAPACITY_EXCEEDED ||
        finalize_fn(instance) != RTFW_STATUS_OK) {
        fprintf(stderr, "runtime setup failed\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    {
        rtfw_memory_plan plan;
        uint64_t summed_bytes;
        memory_plan_init_fn(&plan);
        plan.reserved[2] = 1;
        if (get_memory_plan_fn(instance, &plan) !=
            RTFW_STATUS_INVALID_ARGUMENT) {
            fprintf(stderr, "memory plan accepted a nonzero reserved field\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        memory_plan_init_fn(&plan);
        if (get_memory_plan_fn(instance, &plan) != RTFW_STATUS_OK) {
            fprintf(stderr, "finalized memory plan was unavailable\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        summed_bytes = plan.runtime_control_bytes +
            plan.executor_control_bytes +
            plan.device_control_bytes +
            plan.phase_scratch_total_bytes +
            plan.task_scratch_total_bytes +
            plan.trace_storage_bytes;
        if (plan.memory_budget_bytes != 1048576 ||
            plan.planned_bytes != summed_bytes ||
            plan.planned_bytes > plan.memory_budget_bytes ||
            plan.phase_count != 2 ||
            plan.phase_scratch_bytes != 64 ||
            plan.phase_scratch_stride != 64 ||
            plan.phase_scratch_total_bytes != 128 ||
            plan.task_scratch_bytes != 32 ||
            plan.task_scratch_stride != 64 ||
            plan.task_scratch_slots != 16 ||
            plan.task_scratch_total_bytes != 1024 ||
            plan.trace_slot_bytes < sizeof(rtfw_trace_event) ||
            plan.trace_storage_bytes !=
                plan.trace_capacity * plan.trace_slot_bytes ||
            plan.queue_slots != 8 ||
            plan.scratch_alignment != 64 ||
            plan.overload_policy != RTFW_OVERLOAD_REJECT_SUBMISSION) {
            fprintf(stderr, "finalized memory plan contract mismatch\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }
    {
        rtfw_observability_metadata metadata;
        observability_metadata_init_fn(&metadata);
        --metadata.struct_size;
        if (get_observability_metadata_fn(instance, &metadata) !=
            RTFW_STATUS_INVALID_ARGUMENT) {
            fprintf(stderr, "observability metadata accepted a short struct\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        observability_metadata_init_fn(&metadata);
        if (get_observability_metadata_fn(instance, &metadata) !=
                RTFW_STATUS_OK ||
            metadata.schema_version !=
                RTFW_OBSERVABILITY_SCHEMA_VERSION ||
            metadata.trace_event_size != sizeof(rtfw_trace_event) ||
            metadata.metric_sample_size != sizeof(rtfw_metric_sample) ||
            metadata.metric_count != RTFW_RUNTIME_METRIC_COUNT ||
            metadata.config_id == 0u ||
            metadata.runtime_id == 0u ||
            metadata.build_id[0] == '\0' ||
            strcmp(metadata.workload_id, "cabi.dynamic") != 0) {
            fprintf(stderr, "observability metadata contract mismatch\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }
    if (start_fn(instance) != RTFW_STATUS_OK) {
        fprintf(stderr, "runtime start failed\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    {
        rtfw_platform_preflight_report report;
        uint32_t degradation_level = UINT32_MAX;
        preflight_report_init_fn(&report);
        report.reserved[0] = 1;
        if (get_preflight_report_fn(instance, &report) !=
            RTFW_STATUS_INVALID_ARGUMENT) {
            fprintf(stderr, "preflight report accepted reserved data\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        preflight_report_init_fn(&report);
        if (get_preflight_report_fn(instance, &report) != RTFW_STATUS_OK ||
            report.mode != RTFW_PLATFORM_PREFLIGHT_DISABLED ||
            report.passed != 1u ||
            report.check_count != 0u ||
            get_degradation_level_fn(instance, &degradation_level) !=
                RTFW_STATUS_OK ||
            degradation_level != 0u) {
            fprintf(stderr, "preflight or degradation query failed\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }
    if (register_fn(
            instance,
            "cabi.too-late",
            test_callback,
            &consumer_probe) !=
            RTFW_STATUS_INVALID_STATE ||
        strstr(last_error_fn(instance), "frozen") == NULL) {
        fprintf(stderr, "runtime accepted late callback registration\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }

    {
        rtfw_periodic_config periodic;
        rtfw_periodic_run_result periodic_result;
        periodic_config_init_fn(&periodic);
        periodic.first_frame_index = 100;
        periodic.frame_count = 2;
        periodic.period_ns = 1000000;
        periodic.relative_deadline_ns = 1000000000;
        periodic.reserved[0] = 1;
        periodic_result_init_fn(&periodic_result);
        if (run_periodic_fn(
                instance,
                &periodic,
                periodic_observer,
                &state,
                &periodic_result) != RTFW_STATUS_INVALID_ARGUMENT) {
            fprintf(stderr, "periodic config accepted reserved data\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        periodic.reserved[0] = 0;
        periodic_result.last_frame.reserved0[0] = 1;
        if (run_periodic_fn(
                instance,
                &periodic,
                periodic_observer,
                &state,
                &periodic_result) != RTFW_STATUS_INVALID_ARGUMENT) {
            fprintf(stderr, "periodic result accepted reserved data\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        periodic_result_init_fn(&periodic_result);
        if (run_periodic_fn(
                instance,
                &periodic,
                periodic_observer,
                &state,
                &periodic_result) != RTFW_STATUS_OK ||
            periodic_result.frames_executed != 2u ||
            periodic_result.deadline_misses != 0u ||
            periodic_result.watchdog_events != 0u ||
            periodic_result.final_degradation_level != 0u ||
            periodic_result.last_frame.frame_index != 101u ||
            periodic_result.next_release_ns !=
                periodic_result.first_release_ns + 2000000u ||
            state.periodic_observations != 2u ||
            state.periodic_errors != 0u) {
            fprintf(stderr, "periodic C ABI contract failed\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }

    for (uint64_t i = 0; i < 5; ++i) {
        frame.frame_index = i;
        rtfw_step_result result;
        result_init_fn(&result);
        if (i == 0) {
            result.reserved1[1] = 1;
            if (step_fn(instance, &frame, &result) !=
                    RTFW_STATUS_INVALID_ARGUMENT) {
                fprintf(stderr, "result accepted a nonzero reserved field\n");
                destroy_fn(instance);
                lib_close(handle);
                return EXIT_FAILURE;
            }
            result_init_fn(&result);
        }
        rtfw_status status = step_fn(instance, &frame, &result);
        if (status != RTFW_STATUS_OK ||
            result.callbacks_executed != 2 ||
            result.watchdog_fired != 0u ||
            result.degradation_level != 0u) {
            fprintf(
                stderr,
                "step %llu failed with status %d\n",
                (unsigned long long)i,
                (int)status);
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }

    rtfw_runtime_state runtime_state = RTFW_STATE_CONFIGURING;
    if (stop_fn(instance) != RTFW_STATUS_OK ||
        get_state_fn(instance, &runtime_state) != RTFW_STATUS_OK ||
        runtime_state != RTFW_STATE_STOPPED ||
        state.calls != 14 ||
        state.range_calls != 28 ||
        state.reduction_result != 10 ||
        state.combine_calls != 21 ||
        state.last_frame != 4 ||
        state.ordering_errors != 0 ||
        state.next_slot != 0) {
        fprintf(stderr, "runtime shutdown or callback validation failed\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }

    {
        rtfw_metric_snapshot cumulative;
        rtfw_metric_snapshot first_interval;
        rtfw_metric_snapshot empty_interval;
        rtfw_metric_cursor metric_cursor;
        metric_snapshot_init_fn(&cumulative);
        metric_cursor_init_fn(&metric_cursor);
        metric_cursor.reserved[0] = 1u;
        if (get_metrics_fn(
                instance,
                RTFW_METRIC_INTERVAL,
                &metric_cursor,
                &cumulative) != RTFW_STATUS_INVALID_ARGUMENT) {
            fprintf(stderr, "metric cursor accepted reserved data\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        metric_cursor_init_fn(&metric_cursor);
        metric_cursor.counters[RTFW_METRIC_FRAMES_STARTED] = 1u;
        metric_snapshot_init_fn(&cumulative);
        if (get_metrics_fn(
                instance,
                RTFW_METRIC_INTERVAL,
                &metric_cursor,
                &cumulative) != RTFW_STATUS_INVALID_ARGUMENT) {
            fprintf(stderr, "metric cursor accepted a nonzero fresh baseline\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        metric_cursor_init_fn(&metric_cursor);
        metric_snapshot_init_fn(&cumulative);
        if (get_metrics_fn(
                instance,
                RTFW_METRIC_CUMULATIVE,
                NULL,
                &cumulative) != RTFW_STATUS_OK ||
            cumulative.sample_count != RTFW_RUNTIME_METRIC_COUNT ||
            cumulative.samples[RTFW_METRIC_FRAMES_COMPLETED].value != 7u ||
            cumulative.samples[RTFW_METRIC_CALLBACKS_COMPLETED].value !=
                14u ||
            cumulative.samples[RTFW_METRIC_FRAMES_COMPLETED].kind !=
                RTFW_METRIC_COUNTER ||
            cumulative.samples[RTFW_METRIC_DEGRADATION_LEVEL].kind !=
                RTFW_METRIC_GAUGE) {
            fprintf(stderr, "cumulative metric C ABI contract failed\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        metric_snapshot_init_fn(&first_interval);
        if (get_metrics_fn(
                instance,
                RTFW_METRIC_INTERVAL,
                &metric_cursor,
                &first_interval) != RTFW_STATUS_OK ||
            first_interval.samples[RTFW_METRIC_FRAMES_COMPLETED].value !=
                cumulative.samples[RTFW_METRIC_FRAMES_COMPLETED].value) {
            fprintf(stderr, "first interval metric snapshot failed\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        metric_snapshot_init_fn(&empty_interval);
        if (get_metrics_fn(
                instance,
                RTFW_METRIC_INTERVAL,
                &metric_cursor,
                &empty_interval) != RTFW_STATUS_OK ||
            empty_interval.samples[RTFW_METRIC_FRAMES_COMPLETED].value !=
                0u ||
            empty_interval.samples[RTFW_METRIC_CALLBACKS_COMPLETED].value !=
                0u ||
            empty_interval.samples[RTFW_METRIC_DEGRADATION_LEVEL].value !=
                cumulative.samples[RTFW_METRIC_DEGRADATION_LEVEL].value) {
            fprintf(stderr, "empty interval metric snapshot failed\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }

    {
        rtfw_trace_cursor trace_cursor;
        rtfw_trace_read_result trace_result;
        rtfw_trace_event events[64];
        trace_cursor_init_fn(&trace_cursor);
        trace_read_result_init_fn(&trace_result);
        trace_cursor.next_sequence = 1u;
        if (read_trace_fn(
                instance,
                &trace_cursor,
                events,
                64,
                &trace_result) != RTFW_STATUS_INVALID_ARGUMENT) {
            fprintf(stderr, "trace cursor accepted a nonzero fresh position\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        trace_cursor_init_fn(&trace_cursor);
        trace_read_result_init_fn(&trace_result);
        if (read_trace_fn(
                instance,
                &trace_cursor,
                events,
                64,
                &trace_result) != RTFW_STATUS_OK ||
            trace_result.events_read == 0u ||
            trace_result.remaining_sequence_count != 0u ||
            trace_result.metadata.schema_version !=
                RTFW_OBSERVABILITY_SCHEMA_VERSION ||
            strcmp(
                trace_result.metadata.workload_id,
                "cabi.dynamic") != 0 ||
            events[0].schema_version !=
                RTFW_OBSERVABILITY_SCHEMA_VERSION ||
            events[0].record_size != sizeof(rtfw_trace_event)) {
            fprintf(stderr, "trace cursor C ABI contract failed\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        trace_read_result_init_fn(&trace_result);
        if (read_trace_fn(
                instance,
                &trace_cursor,
                events,
                64,
                &trace_result) != RTFW_STATUS_OK ||
            trace_result.events_read != 0u ||
            trace_result.lost_events != 0u) {
            fprintf(stderr, "trace cursor did not resume cleanly\n");
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }

    destroy_fn(instance);
    destroy_fn(NULL);

    {
        rtfw_handle *replay_instance = NULL;
        rtfw_config replay_config = config;
        unsigned char state_bytes[8] = {
            1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
        const unsigned char checkpoint_state[8] = {
            1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
        const unsigned char payload[8] = {
            11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u};
        unsigned char checkpoint[4096];
        unsigned char input_log[4096];
        uint64_t checkpoint_bytes = 0;
        uint64_t state_hash = 0;
        rtfw_artifact_write_result checkpoint_result;
        rtfw_artifact_write_result input_result;
        rtfw_checkpoint_metadata checkpoint_metadata;
        rtfw_input_log_metadata input_metadata;
        rtfw_replay_input_record record;
        rtfw_replay_result replay_result;
        char unterminated_name[RTFW_REPLAY_IDENTIFIER_CAPACITY];

        replay_config.determinism_tier =
            RTFW_DETERMINISM_D1_SCHEDULE_INDEPENDENT;
        memset(
            unterminated_name,
            'a',
            sizeof(unterminated_name));
        if (create_fn(&replay_config, &replay_instance) !=
                RTFW_STATUS_OK ||
            register_state_fn(
                replay_instance,
                unterminated_name,
                1u,
                state_bytes,
                sizeof(state_bytes)) !=
                RTFW_STATUS_INVALID_ARGUMENT ||
            register_state_fn(
                replay_instance,
                "cabi.state",
                1u,
                state_bytes,
                sizeof(state_bytes)) != RTFW_STATUS_OK ||
            finalize_fn(replay_instance) != RTFW_STATUS_OK ||
            start_fn(replay_instance) != RTFW_STATUS_OK ||
            checkpoint_size_fn(
                replay_instance,
                &checkpoint_bytes) != RTFW_STATUS_OK ||
            checkpoint_bytes > sizeof(checkpoint)) {
            fprintf(stderr, "C ABI replay setup failed\n");
            destroy_fn(replay_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }

        artifact_result_init_fn(&checkpoint_result);
        if (checkpoint_write_fn(
                replay_instance,
                0u,
                NULL,
                0u,
                &checkpoint_result) !=
                RTFW_STATUS_CAPACITY_EXCEEDED ||
            checkpoint_result.required_bytes != checkpoint_bytes) {
            fprintf(stderr, "checkpoint sizing result failed\n");
            destroy_fn(replay_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        artifact_result_init_fn(&checkpoint_result);
        checkpoint_metadata_init_fn(&checkpoint_metadata);
        if (checkpoint_write_fn(
                replay_instance,
                0u,
                checkpoint,
                sizeof(checkpoint),
                &checkpoint_result) != RTFW_STATUS_OK ||
            checkpoint_result.bytes_written != checkpoint_bytes ||
            checkpoint_inspect_fn(
                checkpoint,
                checkpoint_result.bytes_written,
                &checkpoint_metadata) != RTFW_STATUS_OK ||
            checkpoint_metadata.state_count != 1u ||
            checkpoint_metadata.determinism_tier !=
                RTFW_DETERMINISM_D1_SCHEDULE_INDEPENDENT) {
            fprintf(stderr, "checkpoint C ABI contract failed\n");
            destroy_fn(replay_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }

        memset(state_bytes, 0x5a, sizeof(state_bytes));
        checkpoint_metadata_init_fn(&checkpoint_metadata);
        if (checkpoint_restore_fn(
                replay_instance,
                checkpoint,
                checkpoint_result.bytes_written,
                &checkpoint_metadata) != RTFW_STATUS_OK ||
            memcmp(
                state_bytes,
                checkpoint_state,
                sizeof(state_bytes)) != 0) {
            fprintf(stderr, "checkpoint restore C ABI contract failed\n");
            destroy_fn(replay_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }

        replay_record_init_fn(&record);
        record.input_type = 7u;
        record.frame_index = 1u;
        record.delta_ns = 1;
        record.payload = payload;
        record.payload_size = sizeof(payload);
        artifact_result_init_fn(&input_result);
        input_log_metadata_init_fn(&input_metadata);
        if (input_log_write_fn(
                replay_instance,
                &record,
                1u,
                input_log,
                sizeof(input_log),
                &input_result) != RTFW_STATUS_OK ||
            input_log_inspect_fn(
                input_log,
                input_result.bytes_written,
                &input_metadata) != RTFW_STATUS_OK ||
            input_metadata.record_count != 1u ||
            input_metadata.first_frame_index != 1u) {
            fprintf(stderr, "input-log C ABI contract failed\n");
            destroy_fn(replay_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }

        memset(state_bytes, 0x33, sizeof(state_bytes));
        replay_result_init_fn(&replay_result);
        if (replay_fn(
                replay_instance,
                checkpoint,
                checkpoint_result.bytes_written,
                input_log,
                input_result.bytes_written,
                replay_input,
                state_bytes,
                &replay_result) != RTFW_STATUS_OK ||
            replay_result.records_processed != 1u ||
            replay_result.frames_replayed != 1u ||
            memcmp(state_bytes, payload, sizeof(state_bytes)) != 0 ||
            registered_state_hash_fn(
                replay_instance,
                &state_hash) != RTFW_STATUS_OK ||
            state_hash != replay_result.final_state_hash ||
            stop_fn(replay_instance) != RTFW_STATUS_OK) {
            fprintf(stderr, "replay C ABI contract failed\n");
            destroy_fn(replay_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        destroy_fn(replay_instance);
    }

    {
        rtfw_handle *device_instance = NULL;
        rtfw_device_backend_id device_backend =
            RTFW_INVALID_DEVICE_BACKEND_ID;
        rtfw_device_buffer_id device_buffer =
            RTFW_INVALID_DEVICE_BUFFER_ID;
        rtfw_phase_id device_phase = RTFW_INVALID_PHASE_ID;
        rtfw_phase_id stale_phase = RTFW_INVALID_PHASE_ID;
        rtfw_device_backend_api backend_api;
        cabi_device_backend backend_state;
        cabi_device_command command;
        unsigned char device_bytes[16];
        rtfw_frame_context device_frame;
        rtfw_step_result device_result;
        rtfw_device_health device_health;
        rtfw_memory_plan device_plan;
        rtfw_metric_snapshot device_metrics;
        rtfw_runtime_state teardown_state = RTFW_STATE_CONFIGURING;
        rtfw_config device_config = config;

        memset(&backend_api, 0, sizeof(backend_api));
        memset(&backend_state, 0, sizeof(backend_state));
        memset(device_bytes, 0, sizeof(device_bytes));
        backend_api.struct_size = sizeof(backend_api);
        backend_api.abi_version = RTFW_DEVICE_ABI_VERSION;
        backend_api.instance = &backend_state;
        backend_api.get_capabilities = cabi_device_capabilities;
        backend_api.initialize = cabi_device_initialize;
        backend_api.register_buffer = cabi_device_register_buffer;
        backend_api.unregister_buffer = cabi_device_unregister_buffer;
        backend_api.submit = cabi_device_submit;
        backend_api.poll = cabi_device_poll;
        backend_api.cancel = cabi_device_cancel;
        backend_api.get_health = cabi_device_health;
        backend_api.reset = cabi_device_reset;
        backend_api.shutdown = cabi_device_shutdown;

        if (create_fn(&device_config, &device_instance) !=
                RTFW_STATUS_OK) {
            fprintf(stderr, "C ABI device runtime creation failed\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        --backend_api.struct_size;
        if (register_device_backend_fn(
                device_instance,
                "cabi.device.short",
                &backend_api,
                &device_backend) != RTFW_STATUS_INVALID_ARGUMENT ||
            device_backend != RTFW_INVALID_DEVICE_BACKEND_ID) {
            fprintf(stderr, "C ABI device backend accepted a short table\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        backend_api.struct_size = sizeof(backend_api);
        if (register_device_backend_fn(
                device_instance,
                "cabi.device",
                &backend_api,
                &device_backend) != RTFW_STATUS_OK ||
            device_backend == RTFW_INVALID_DEVICE_BACKEND_ID ||
            register_device_buffer_fn(
                device_instance,
                "cabi.buffer",
                device_backend,
                device_bytes,
                sizeof(device_bytes),
                RTFW_DEVICE_BUFFER_HOST_READ |
                    RTFW_DEVICE_BUFFER_HOST_WRITE |
                    RTFW_DEVICE_BUFFER_DEVICE_READ |
                    RTFW_DEVICE_BUFFER_DEVICE_WRITE,
                &device_buffer) != RTFW_STATUS_OK ||
            device_buffer == RTFW_INVALID_DEVICE_BUFFER_ID) {
            fprintf(stderr, "C ABI device registration failed\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        command.buffer = device_buffer;
        command.bytes = sizeof(device_bytes);
        if (register_device_phase_fn(
                device_instance,
                "cabi.device.fill",
                device_backend,
                cabi_device_command_callback,
                &command,
                &device_phase) != RTFW_STATUS_OK ||
            device_phase == RTFW_INVALID_PHASE_ID ||
            finalize_fn(device_instance) != RTFW_STATUS_OK) {
            fprintf(stderr, "C ABI device phase setup failed\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        memory_plan_init_fn(&device_plan);
        if (get_memory_plan_fn(device_instance, &device_plan) !=
                RTFW_STATUS_OK ||
            device_plan.device_backend_count != 1u ||
            device_plan.device_buffer_count != 1u ||
            device_plan.device_control_bytes == 0u ||
            device_plan.device_backend_reported_bytes == 0u ||
            start_fn(device_instance) != RTFW_STATUS_OK) {
            fprintf(stderr, "C ABI device memory/start contract failed\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }

        frame_init_fn(&device_frame);
        result_init_fn(&device_result);
        device_frame.frame_index = 9u;
        device_frame.delta_ns = 1000000;
        if (step_fn(
                device_instance,
                &device_frame,
                &device_result) != RTFW_STATUS_OK ||
            device_result.callbacks_executed != 1u) {
            fprintf(stderr, "C ABI device step failed\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        {
            size_t index;
            for (index = 0; index < sizeof(device_bytes); ++index) {
                if (device_bytes[index] != 0x6bu) {
                    fprintf(stderr, "C ABI device buffer was not updated\n");
                    destroy_fn(device_instance);
                    lib_close(handle);
                    return EXIT_FAILURE;
                }
            }
        }

        device_health_init_fn(&device_health);
        device_health.reserved[0] = 1u;
        if (get_device_health_fn(
                device_instance,
                device_backend,
                &device_health) != RTFW_STATUS_INVALID_ARGUMENT) {
            fprintf(stderr, "C ABI device health accepted reserved data\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        device_health_init_fn(&device_health);
        if (get_device_health_fn(
                device_instance,
                (rtfw_device_backend_id)device_buffer,
                &device_health) != RTFW_STATUS_INVALID_HANDLE) {
            fprintf(stderr, "C ABI device handle kinds were not isolated\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        device_health_init_fn(&device_health);
        metric_snapshot_init_fn(&device_metrics);
        if (get_device_health_fn(
                device_instance,
                device_backend,
                &device_health) != RTFW_STATUS_OK ||
            device_health.state != RTFW_DEVICE_HEALTH_HEALTHY ||
            device_health.submissions != 1u ||
            device_health.completions != 1u ||
            reset_device_fn(
                device_instance,
                device_backend) != RTFW_STATUS_OK ||
            get_metrics_fn(
                device_instance,
                RTFW_METRIC_CUMULATIVE,
                NULL,
                &device_metrics) != RTFW_STATUS_OK ||
            device_metrics.samples[
                RTFW_METRIC_DEVICE_SUBMISSIONS].value != 1u ||
            device_metrics.samples[
                RTFW_METRIC_DEVICE_COMPLETIONS].value != 1u ||
            device_metrics.samples[
                RTFW_METRIC_DEVICE_RESETS].value != 1u) {
            fprintf(stderr, "C ABI device health/reset failed\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }

        /*
         * ABI v8 destroy is void, so failed device teardown must retain the
         * handle and borrowed objects. A later stop retries only unresolved
         * ownership: the successfully unregistered buffer is not repeated,
         * while the failed backend shutdown remains retryable.
         */
        backend_state.shutdown_failures_remaining = 2u;
        if (stop_fn(device_instance) != RTFW_STATUS_DEVICE_ERROR ||
            get_state_fn(device_instance, &teardown_state) !=
                RTFW_STATUS_OK ||
            teardown_state != RTFW_STATE_RUNNING ||
            backend_state.unregisters != 1u ||
            backend_state.shutdown_attempts != 1u ||
            backend_state.shutdowns != 0u ||
            backend_state.buffer != NULL ||
            backend_state.initialized == 0 ||
            strstr(
                last_error_fn(device_instance),
                "device teardown failed") == NULL) {
            fprintf(stderr, "C ABI device stop failure was not recoverable\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        if (register_phase_fn(
                device_instance,
                NULL,
                test_callback,
                NULL,
                &stale_phase) != RTFW_STATUS_INVALID_ARGUMENT ||
            stale_phase != RTFW_INVALID_PHASE_ID ||
            strstr(
                last_error_fn(device_instance),
                "callback name") == NULL) {
            fprintf(stderr, "C ABI teardown stale-error setup failed\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        destroy_fn(device_instance);
        if (backend_state.unregisters != 1u ||
            backend_state.shutdown_attempts != 2u ||
            backend_state.shutdowns != 0u ||
            backend_state.initialized == 0 ||
            get_state_fn(device_instance, &teardown_state) !=
                RTFW_STATUS_OK ||
            teardown_state != RTFW_STATE_RUNNING ||
            strstr(
                last_error_fn(device_instance),
                "device teardown failed") == NULL) {
            fprintf(
                stderr,
                "C ABI destroy did not retain failed teardown "
                "(unregisters=%llu shutdown_attempts=%llu "
                "shutdowns=%llu initialized=%d state=%u error='%s')\n",
                (unsigned long long)backend_state.unregisters,
                (unsigned long long)backend_state.shutdown_attempts,
                (unsigned long long)backend_state.shutdowns,
                backend_state.initialized,
                (unsigned int)teardown_state,
                last_error_fn(device_instance));
            lib_close(handle);
            return EXIT_FAILURE;
        }
        if (stop_fn(device_instance) != RTFW_STATUS_OK ||
            get_state_fn(device_instance, &teardown_state) !=
                RTFW_STATUS_OK ||
            teardown_state != RTFW_STATE_STOPPED ||
            backend_state.unregisters != 1u ||
            backend_state.shutdown_attempts != 3u ||
            backend_state.shutdowns != 1u ||
            backend_state.initialized != 0 ||
            last_error_fn(device_instance)[0] != '\0') {
            fprintf(stderr, "C ABI device teardown retry failed\n");
            destroy_fn(device_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        destroy_fn(device_instance);
    }

    {
        rtfw_handle *conflict_instance = NULL;
        rtfw_phase_id conflict_writer = RTFW_INVALID_PHASE_ID;
        rtfw_phase_id conflict_reader = RTFW_INVALID_PHASE_ID;
        rtfw_resource_id conflict_resource = RTFW_INVALID_RESOURCE_ID;
        if (create_fn(&config, &conflict_instance) != RTFW_STATUS_OK ||
            register_phase_fn(
                conflict_instance,
                "conflict.writer",
                test_callback,
                &producer_probe,
                &conflict_writer) != RTFW_STATUS_OK ||
            register_phase_fn(
                conflict_instance,
                "conflict.reader",
                test_callback,
                &consumer_probe,
                &conflict_reader) != RTFW_STATUS_OK ||
            register_resource_fn(
                conflict_instance,
                "conflict.state",
                &conflict_resource) != RTFW_STATUS_OK ||
            declare_access_fn(
                conflict_instance,
                conflict_writer,
                conflict_resource,
                RTFW_RESOURCE_WRITE) != RTFW_STATUS_OK ||
            declare_access_fn(
                conflict_instance,
                conflict_reader,
                conflict_resource,
                RTFW_RESOURCE_READ) != RTFW_STATUS_OK ||
            finalize_fn(conflict_instance) != RTFW_STATUS_RESOURCE_CONFLICT ||
            strstr(last_error_fn(conflict_instance), "conflict.state") == NULL) {
            fprintf(stderr, "C ABI resource conflict validation failed\n");
            destroy_fn(conflict_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        destroy_fn(conflict_instance);
    }

    {
        rtfw_handle *malformed_instance = NULL;
        rtfw_frame_context malformed_frame;
        if (create_fn(&config, &malformed_instance) != RTFW_STATUS_OK ||
            register_fn(
                malformed_instance,
                "malformed-result",
                malformed_result_callback,
                NULL) != RTFW_STATUS_OK ||
            finalize_fn(malformed_instance) != RTFW_STATUS_OK ||
            start_fn(malformed_instance) != RTFW_STATUS_OK) {
            fprintf(stderr, "malformed callback result setup failed\n");
            destroy_fn(malformed_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        frame_init_fn(&malformed_frame);
        malformed_frame.delta_ns = 1000000;
        if (step_fn(malformed_instance, &malformed_frame, NULL) !=
            RTFW_STATUS_CALLBACK_FAILED) {
            fprintf(stderr, "malformed callback result was not rejected\n");
            destroy_fn(malformed_instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
        destroy_fn(malformed_instance);
    }

    if (lib_close(handle) != 0) {
        fprintf(stderr, "failed to unload runtime: %s\n", lib_error());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

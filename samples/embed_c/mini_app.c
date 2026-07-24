#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rt/c_api.h>

typedef struct sample_state {
    uint64_t produced;
    uint64_t consumed;
    uint64_t last_frame;
    uint64_t lanes[64];
} sample_state;

static void store_u64_le(uint8_t* output, uint64_t value) {
    size_t index;
    for (index = 0; index < sizeof(value); ++index) {
        output[index] = (uint8_t)(value >> (index * 8u));
    }
}

static rtfw_callback_result produce_range(
    void* user_data,
    const rtfw_task_context* context,
    uint64_t begin,
    uint64_t end,
    uint64_t task_index) {
    sample_state* state = (sample_state*)user_data;
    uint64_t worker_index = 0;
    uint64_t scratch_bytes = 0;
    void* scratch = NULL;
    (void)task_index;
    if (!state || !context ||
        rtfw_task_worker_index(context, &worker_index) != RTFW_STATUS_OK ||
        rtfw_task_scratch(context, &scratch, &scratch_bytes) !=
            RTFW_STATUS_OK ||
        !scratch ||
        scratch_bytes < sizeof(uint64_t) ||
        ((uintptr_t)scratch % 64u) != 0u) {
        return RTFW_CALLBACK_ERROR;
    }
    ((uint8_t*)scratch)[0] = 0x17u;
    (void)worker_index;
    for (uint64_t index = begin; index < end; ++index) {
        state->lanes[index] = state->produced;
    }
    return RTFW_CALLBACK_OK;
}

static rtfw_callback_result produce(
    void* user_data,
    const rtfw_callback_context* context) {
    sample_state* state = (sample_state*)user_data;
    if (!state || !context ||
        context->struct_size < sizeof(rtfw_callback_context) ||
        context->delta_ns != 2000000 ||
        context->scratch_bytes < sizeof(uint64_t)) {
        return RTFW_CALLBACK_ERROR;
    }

    ++state->produced;
    state->last_frame = context->frame_index;
    memcpy(context->scratch, &state->produced, sizeof(state->produced));
    return rtfw_parallel_for(
               context->tasks,
               64,
               8,
               produce_range,
               state) == RTFW_STATUS_OK
        ? RTFW_CALLBACK_OK
        : RTFW_CALLBACK_ERROR;
}

static rtfw_callback_result consume(
    void* user_data,
    const rtfw_callback_context* context) {
    sample_state* state = (sample_state*)user_data;
    if (!state || !context ||
        context->struct_size < sizeof(rtfw_callback_context) ||
        context->delta_ns != 2000000 ||
        context->scratch_bytes < sizeof(uint64_t)) {
        return RTFW_CALLBACK_ERROR;
    }
    if (state->produced != state->consumed + 1) {
        return RTFW_CALLBACK_ERROR;
    }
    for (uint64_t index = 0; index < 64; ++index) {
        if (state->lanes[index] != state->produced) {
            return RTFW_CALLBACK_ERROR;
        }
    }
    ++state->consumed;
    return RTFW_CALLBACK_OK;
}

static int check_status(
    rtfw_handle* handle,
    rtfw_status status,
    const char* operation) {
    if (status == RTFW_STATUS_OK) {
        return 1;
    }
    fprintf(
        stderr,
        "mini_app: %s failed (%d): %s\n",
        operation,
        (int)status,
        handle ? rtfw_last_error(handle) : rtfw_status_message(status));
    return 0;
}

int main(void) {
    printf(
        "mini_app: runtime version %u.%u.%u\n",
        rt_version_major(),
        rt_version_minor(),
        rt_version_patch());

    rtfw_config config;
    rtfw_config_init(&config);
    if (rtfw_config_set(&config, "callback_capacity", "2") != RTFW_STATUS_OK ||
        rtfw_config_set(&config, "scratch_bytes", "128") != RTFW_STATUS_OK ||
        rtfw_config_set(&config, "trace_capacity", "32") != RTFW_STATUS_OK ||
        rtfw_config_set(
            &config,
            "executor_policy",
            "bounded_throughput") != RTFW_STATUS_OK ||
        rtfw_config_set(&config, "worker_count", "4") != RTFW_STATUS_OK ||
        rtfw_config_set(
            &config,
            "executor_queue_capacity",
            "128") != RTFW_STATUS_OK ||
        rtfw_config_set(
            &config,
            "scratch_alignment",
            "64") != RTFW_STATUS_OK ||
        rtfw_config_set(
            &config,
            "task_scratch_bytes",
            "64") != RTFW_STATUS_OK ||
        rtfw_config_set(
            &config,
            "task_scratch_slots",
            "128") != RTFW_STATUS_OK ||
        rtfw_config_set(
            &config,
            "memory_budget_bytes",
            "1048576") != RTFW_STATUS_OK ||
        rtfw_config_set(
            &config,
            "overload_policy",
            "fail_frame") != RTFW_STATUS_OK ||
        rtfw_config_set(
            &config,
            "workload_id",
            "sample.embed_c") != RTFW_STATUS_OK) {
        fprintf(stderr, "mini_app: failed to build configuration\n");
        return EXIT_FAILURE;
    }

    rtfw_handle* runtime = NULL;
    if (!check_status(
            runtime,
            rtfw_create(&config, &runtime),
            "create")) {
        return EXIT_FAILURE;
    }

    sample_state state = {0};
    uint8_t canonical_state[24] = {0};
    rtfw_phase_id consumer_phase = RTFW_INVALID_PHASE_ID;
    rtfw_phase_id producer_phase = RTFW_INVALID_PHASE_ID;
    rtfw_resource_id simulation_state = RTFW_INVALID_RESOURCE_ID;
    if (!check_status(
            runtime,
            rtfw_register_phase(
                runtime,
                "sample.consume",
                consume,
                &state,
                &consumer_phase),
            "register consumer") ||
        !check_status(
            runtime,
            rtfw_register_phase(
                runtime,
                "sample.produce",
                produce,
                &state,
                &producer_phase),
            "register producer") ||
        !check_status(
            runtime,
            rtfw_register_resource(
                runtime,
                "sample.simulation-state",
                &simulation_state),
            "register resource") ||
        !check_status(
            runtime,
            rtfw_register_state(
                runtime,
                "sample.canonical-state",
                1,
                canonical_state,
                sizeof(canonical_state)),
            "register replay state") ||
        !check_status(
            runtime,
            rtfw_add_dependency(
                runtime,
                producer_phase,
                consumer_phase),
            "add dependency") ||
        !check_status(
            runtime,
            rtfw_declare_resource_access(
                runtime,
                producer_phase,
                simulation_state,
                RTFW_RESOURCE_WRITE),
            "declare producer access") ||
        !check_status(
            runtime,
            rtfw_declare_resource_access(
                runtime,
                consumer_phase,
                simulation_state,
                RTFW_RESOURCE_READ),
            "declare consumer access") ||
        !check_status(runtime, rtfw_finalize(runtime), "finalize")) {
        rtfw_destroy(runtime);
        return EXIT_FAILURE;
    }

    rtfw_memory_plan memory_plan;
    rtfw_memory_plan_init(&memory_plan);
    if (!check_status(
            runtime,
            rtfw_get_memory_plan(runtime, &memory_plan),
            "read memory plan") ||
        memory_plan.planned_bytes > memory_plan.memory_budget_bytes ||
        memory_plan.task_scratch_bytes != 64 ||
        memory_plan.state_count != 1 ||
        memory_plan.overload_policy != RTFW_OVERLOAD_FAIL_FRAME ||
        !check_status(runtime, rtfw_start(runtime), "start")) {
        rtfw_destroy(runtime);
        return EXIT_FAILURE;
    }

    rtfw_periodic_config periodic;
    rtfw_periodic_config_init(&periodic);
    periodic.frame_count = 5;
    periodic.period_ns = 2000000;
    periodic.relative_deadline_ns = 1000000000u;
    rtfw_periodic_run_result periodic_result;
    rtfw_periodic_run_result_init(&periodic_result);
    if (!check_status(
            runtime,
            rtfw_run_periodic(
                runtime,
                &periodic,
                NULL,
                NULL,
                &periodic_result),
            "run periodic") ||
        periodic_result.frames_executed != 5 ||
        periodic_result.deadline_misses != 0 ||
        periodic_result.watchdog_events != 0) {
        rtfw_destroy(runtime);
        return EXIT_FAILURE;
    }

    if (!check_status(runtime, rtfw_stop(runtime), "stop") ||
        state.produced != 5 ||
        state.consumed != 5 ||
        state.last_frame != 4) {
        rtfw_destroy(runtime);
        return EXIT_FAILURE;
    }

    uint8_t checkpoint[1024];
    rtfw_artifact_write_result checkpoint_result;
    rtfw_checkpoint_metadata checkpoint_metadata;
    store_u64_le(canonical_state, state.produced);
    store_u64_le(canonical_state + 8, state.consumed);
    store_u64_le(canonical_state + 16, state.last_frame);
    rtfw_artifact_write_result_init(&checkpoint_result);
    rtfw_checkpoint_metadata_init(&checkpoint_metadata);
    if (!check_status(
            runtime,
            rtfw_checkpoint_write(
                runtime,
                state.last_frame,
                checkpoint,
                sizeof(checkpoint),
                &checkpoint_result),
            "write checkpoint") ||
        !check_status(
            runtime,
            rtfw_checkpoint_inspect(
                checkpoint,
                checkpoint_result.bytes_written,
                &checkpoint_metadata),
            "inspect checkpoint") ||
        checkpoint_metadata.state_count != 1) {
        rtfw_destroy(runtime);
        return EXIT_FAILURE;
    }

    rtfw_metric_snapshot metrics;
    rtfw_trace_cursor trace_cursor;
    rtfw_trace_read_result trace_result;
    rtfw_trace_event trace_events[32];
    rtfw_metric_snapshot_init(&metrics);
    rtfw_trace_cursor_init(&trace_cursor);
    rtfw_trace_read_result_init(&trace_result);
    if (!check_status(
            runtime,
            rtfw_get_metrics(
                runtime,
                RTFW_METRIC_CUMULATIVE,
                NULL,
                &metrics),
            "read metrics") ||
        metrics.samples[RTFW_METRIC_FRAMES_COMPLETED].value != 5 ||
        metrics.samples[RTFW_METRIC_CALLBACKS_COMPLETED].value != 10 ||
        !check_status(
            runtime,
            rtfw_read_trace(
                runtime,
                &trace_cursor,
                trace_events,
                32,
                &trace_result),
            "read trace") ||
        trace_result.events_read == 0 ||
        strcmp(
            metrics.metadata.workload_id,
            "sample.embed_c") != 0) {
        rtfw_destroy(runtime);
        return EXIT_FAILURE;
    }

    rtfw_destroy(runtime);
    printf(
        "mini_app: executed 5 absolute-cadence graph frames with %llu "
        "planned runtime bytes; observability schema %u retained %llu "
        "events; checkpoint state hash %llu\n",
        (unsigned long long)memory_plan.planned_bytes,
        metrics.metadata.schema_version,
        (unsigned long long)trace_result.events_read,
        (unsigned long long)checkpoint_metadata.state_hash);
    return EXIT_SUCCESS;
}

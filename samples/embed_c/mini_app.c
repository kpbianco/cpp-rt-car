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

static rtfw_callback_result produce_range(
    void* user_data,
    const rtfw_task_context* context,
    uint64_t begin,
    uint64_t end,
    uint64_t task_index) {
    sample_state* state = (sample_state*)user_data;
    uint64_t worker_index = 0;
    (void)task_index;
    if (!state || !context ||
        rtfw_task_worker_index(context, &worker_index) != RTFW_STATUS_OK) {
        return RTFW_CALLBACK_ERROR;
    }
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
            "128") != RTFW_STATUS_OK) {
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
        !check_status(runtime, rtfw_finalize(runtime), "finalize") ||
        !check_status(runtime, rtfw_start(runtime), "start")) {
        rtfw_destroy(runtime);
        return EXIT_FAILURE;
    }

    for (uint64_t frame_index = 0; frame_index < 5; ++frame_index) {
        rtfw_frame_context frame;
        rtfw_frame_context_init(&frame);
        frame.frame_index = frame_index;
        frame.delta_ns = 2000000;

        uint64_t now_ns = 0;
        if (!check_status(
                runtime,
                rtfw_now_ns(runtime, &now_ns),
                "read clock")) {
            rtfw_destroy(runtime);
            return EXIT_FAILURE;
        }
        frame.has_deadline = 1;
        frame.deadline_ns = now_ns + 1000000000u;

        rtfw_step_result result;
        rtfw_step_result_init(&result);
        if (!check_status(
                runtime,
                rtfw_step(runtime, &frame, &result),
                "step") ||
            result.callbacks_executed != 2 ||
            result.deadline_missed) {
            rtfw_destroy(runtime);
            return EXIT_FAILURE;
        }
    }

    if (!check_status(runtime, rtfw_stop(runtime), "stop") ||
        state.produced != 5 ||
        state.consumed != 5 ||
        state.last_frame != 4) {
        rtfw_destroy(runtime);
        return EXIT_FAILURE;
    }

    rtfw_destroy(runtime);
    printf("mini_app: executed 5 compiled graph frames\n");
    return EXIT_SUCCESS;
}

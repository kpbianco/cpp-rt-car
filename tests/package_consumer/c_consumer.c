#include <rt/c_api.h>

#include <stdint.h>

int main(void) {
    rtfw_abi_info abi;
    rtfw_abi_info_init(&abi);
    if (rtfw_get_abi_info(&abi) != RTFW_STATUS_OK ||
        abi.abi_version != RTFW_C_ABI_VERSION ||
        abi.layout_fingerprint != RTFW_C_ABI_LAYOUT_FINGERPRINT ||
        rtfw_check_abi(
            RTFW_C_ABI_VERSION,
            RTFW_C_ABI_LAYOUT_FINGERPRINT) != RTFW_STATUS_OK) {
        return 1;
    }

    rtfw_config config;
    rtfw_config_init(&config);
    rtfw_handle* runtime = 0;
    if (rtfw_create(&config, &runtime) != RTFW_STATUS_OK ||
        rtfw_finalize(runtime) != RTFW_STATUS_OK ||
        rtfw_start(runtime) != RTFW_STATUS_OK) {
        rtfw_destroy(runtime);
        return 2;
    }

    rtfw_frame_context frame;
    rtfw_frame_context_init(&frame);
    frame.frame_index = UINT64_C(1);
    frame.delta_ns = INT64_C(1000000);
    if (rtfw_step(runtime, &frame, 0) != RTFW_STATUS_OK ||
        rtfw_stop(runtime) != RTFW_STATUS_OK) {
        rtfw_destroy(runtime);
        return 3;
    }
    rtfw_destroy(runtime);
    return 0;
}

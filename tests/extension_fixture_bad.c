#include <rt/extension_abi.h>

#if defined(_WIN32)
#define RTFW_FIXTURE_EXPORT __declspec(dllexport)
#else
#define RTFW_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

static rtfw_callback_result bad_phase(
    void* user_data,
    const rtfw_callback_context* context) {
    (void)user_data;
    (void)context;
    return RTFW_CALLBACK_OK;
}

RTFW_FIXTURE_EXPORT rtfw_status RTFW_EXTENSION_CALL
rtfw_extension_bad_entry_v1(
    const rtfw_extension_host_api_v1* host,
    rtfw_extension_descriptor_v1* descriptor) {
    rtfw_extension_phase_v1 phase = {0};
    rtfw_extension_handle_v1 handle = {0};
    phase.struct_size = RTFW_EXTENSION_PHASE_V1_REQUIRED_SIZE;
    phase.abi_version = RTFW_EXTENSION_ABI_VERSION;
    phase.name[0] = 'b'; phase.name[1] = 'a'; phase.name[2] = 'd';
    phase.callback = bad_phase;
    if (host->stage_phase(host->context, &phase, &handle) != RTFW_STATUS_OK) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    descriptor->struct_size =
        RTFW_EXTENSION_DESCRIPTOR_V1_REQUIRED_SIZE - 1u;
    descriptor->current_abi_version = RTFW_EXTENSION_ABI_VERSION;
    descriptor->min_compatible_abi_version = RTFW_EXTENSION_ABI_VERSION;
    descriptor->name[0] = 'b'; descriptor->name[1] = 'a';
    descriptor->name[2] = 'd';
    descriptor->version[0] = '1';
    descriptor->phase_count = 1;
    return RTFW_STATUS_OK;
}

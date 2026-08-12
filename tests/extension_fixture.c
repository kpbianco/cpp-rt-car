#include <rt/extension_abi.h>

#include <stddef.h>

#if defined(_WIN32)
#define RTFW_FIXTURE_EXPORT __declspec(dllexport)
#else
#define RTFW_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

_Static_assert(sizeof(rtfw_extension_handle_v1) == 16, "extension handle");
_Static_assert(sizeof(rtfw_extension_phase_v1) == 120, "phase prefix");
_Static_assert(sizeof(rtfw_extension_backend_v1) == 264, "backend prefix");
_Static_assert(sizeof(rtfw_extension_service_status_v1) == 64,
               "service status prefix");
_Static_assert(sizeof(rtfw_extension_service_api_v1) == 88,
               "service API prefix");
_Static_assert(sizeof(rtfw_extension_service_v1) == 264, "service prefix");
_Static_assert(sizeof(rtfw_extension_resource_v1) == 104, "resource prefix");
_Static_assert(sizeof(rtfw_extension_relationship_v1) == 80,
               "relationship prefix");
_Static_assert(sizeof(rtfw_extension_host_api_v1) == 136, "host API prefix");
_Static_assert(sizeof(rtfw_extension_descriptor_v1) == 216,
               "descriptor prefix");
_Static_assert(_Alignof(rtfw_extension_handle_v1) == 4, "handle alignment");
_Static_assert(_Alignof(rtfw_extension_phase_v1) == 8, "phase alignment");
_Static_assert(_Alignof(rtfw_extension_backend_v1) == 8, "backend alignment");
_Static_assert(_Alignof(rtfw_extension_service_status_v1) == 8,
               "service status alignment");
_Static_assert(_Alignof(rtfw_extension_service_api_v1) == 8,
               "service API alignment");
_Static_assert(_Alignof(rtfw_extension_service_v1) == 8,
               "service alignment");
_Static_assert(_Alignof(rtfw_extension_resource_v1) == 8,
               "resource alignment");
_Static_assert(_Alignof(rtfw_extension_relationship_v1) == 8,
               "relationship alignment");
_Static_assert(_Alignof(rtfw_extension_host_api_v1) == 8,
               "host API alignment");
_Static_assert(_Alignof(rtfw_extension_descriptor_v1) == 8,
               "descriptor alignment");
_Static_assert(offsetof(rtfw_extension_handle_v1, kind) == 4,
               "handle kind offset");
_Static_assert(offsetof(rtfw_extension_handle_v1, slot) == 8,
               "handle slot offset");
_Static_assert(offsetof(rtfw_extension_handle_v1, generation) == 12,
               "handle generation offset");
_Static_assert(offsetof(rtfw_extension_phase_v1, callback) == 72,
               "phase callback offset");
_Static_assert(offsetof(rtfw_extension_phase_v1, reserved) == 88,
               "phase reserved offset");
_Static_assert(offsetof(rtfw_extension_backend_v1, api) == 72,
               "backend API offset");
_Static_assert(offsetof(rtfw_extension_backend_v1, reserved) == 232,
               "backend reserved offset");
_Static_assert(offsetof(rtfw_extension_service_status_v1, status) == 16,
               "service status value offset");
_Static_assert(offsetof(rtfw_extension_service_status_v1, observed_value) == 24,
               "service observed value offset");
_Static_assert(offsetof(rtfw_extension_service_api_v1, initialize) == 16,
               "service initialize offset");
_Static_assert(offsetof(rtfw_extension_service_api_v1, reserved) == 56,
               "service API reserved offset");
_Static_assert(offsetof(rtfw_extension_service_v1, interface_name) == 72,
               "service interface offset");
_Static_assert(offsetof(rtfw_extension_service_v1, api) == 144,
               "service API record offset");
_Static_assert(offsetof(rtfw_extension_resource_v1, reserved) == 72,
               "resource reserved offset");
_Static_assert(offsetof(rtfw_extension_relationship_v1, first) == 16,
               "relationship first offset");
_Static_assert(offsetof(rtfw_extension_relationship_v1, second) == 32,
               "relationship second offset");
_Static_assert(offsetof(rtfw_extension_host_api_v1, stage_phase) == 64,
               "host callback offset");
_Static_assert(offsetof(rtfw_extension_host_api_v1, reserved) == 104,
               "host reserved offset");
_Static_assert(offsetof(rtfw_extension_descriptor_v1, phase_count) == 144,
               "descriptor count offset");
_Static_assert(offsetof(rtfw_extension_descriptor_v1, reserved) == 184,
               "descriptor reserved offset");

static uint32_t fixture_calls;

static rtfw_callback_result fixture_phase(
    void* user_data,
    const rtfw_callback_context* context) {
    uint32_t* calls = (uint32_t*)user_data;
    if (context == NULL || context->struct_size != sizeof(*context)) {
        return RTFW_CALLBACK_ERROR;
    }
    ++*calls;
    return RTFW_CALLBACK_OK;
}

RTFW_FIXTURE_EXPORT uint32_t rtfw_extension_fixture_calls(void) {
    return fixture_calls;
}

RTFW_FIXTURE_EXPORT void rtfw_extension_fixture_reset(void) {
    fixture_calls = 0;
}

RTFW_FIXTURE_EXPORT rtfw_status RTFW_EXTENSION_CALL rtfw_extension_entry_v1(
    const rtfw_extension_host_api_v1* host,
    rtfw_extension_descriptor_v1* descriptor) {
    rtfw_extension_phase_v1 phase = {0};
    rtfw_extension_resource_v1 resource = {0};
    rtfw_extension_relationship_v1 relationship = {0};
    rtfw_extension_handle_v1 phase_handle = {0};
    rtfw_extension_handle_v1 resource_handle = {0};
    if (host == NULL || descriptor == NULL ||
        host->struct_size != RTFW_EXTENSION_HOST_API_V1_REQUIRED_SIZE ||
        host->current_abi_version != RTFW_EXTENSION_ABI_VERSION ||
        host->min_compatible_abi_version !=
            RTFW_EXTENSION_ABI_MIN_COMPATIBLE_VERSION) {
        return RTFW_STATUS_INCOMPATIBLE_ABI;
    }
    phase.struct_size = RTFW_EXTENSION_PHASE_V1_REQUIRED_SIZE;
    phase.abi_version = RTFW_EXTENSION_ABI_VERSION;
    phase.name[0] = 'c'; phase.name[1] = '.'; phase.name[2] = 'p';
    phase.name[3] = 'h'; phase.name[4] = 'a'; phase.name[5] = 's';
    phase.name[6] = 'e';
    phase.callback = fixture_phase;
    phase.user_data = &fixture_calls;
    resource.struct_size = RTFW_EXTENSION_RESOURCE_V1_REQUIRED_SIZE;
    resource.abi_version = RTFW_EXTENSION_ABI_VERSION;
    resource.name[0] = 'c'; resource.name[1] = '.'; resource.name[2] = 'r';
    if (host->stage_phase(host->context, &phase, &phase_handle) !=
            RTFW_STATUS_OK ||
        host->stage_resource(host->context, &resource, &resource_handle) !=
            RTFW_STATUS_OK) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    relationship.struct_size = RTFW_EXTENSION_RELATIONSHIP_V1_REQUIRED_SIZE;
    relationship.abi_version = RTFW_EXTENSION_ABI_VERSION;
    relationship.kind = RTFW_EXTENSION_RELATIONSHIP_PHASE_RESOURCE;
    relationship.access = RTFW_EXTENSION_RESOURCE_ACCESS_WRITE;
    relationship.first = phase_handle;
    relationship.second = resource_handle;
    if (host->stage_relationship(host->context, &relationship) !=
        RTFW_STATUS_OK) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *descriptor = (rtfw_extension_descriptor_v1){0};
    descriptor->struct_size = RTFW_EXTENSION_DESCRIPTOR_V1_REQUIRED_SIZE;
    descriptor->current_abi_version = RTFW_EXTENSION_ABI_VERSION;
    descriptor->min_compatible_abi_version =
        RTFW_EXTENSION_ABI_MIN_COMPATIBLE_VERSION;
    descriptor->name[0] = 'c'; descriptor->name[1] = '.';
    descriptor->name[2] = 'f'; descriptor->name[3] = 'i';
    descriptor->name[4] = 'x'; descriptor->name[5] = 't';
    descriptor->name[6] = 'u'; descriptor->name[7] = 'r';
    descriptor->name[8] = 'e';
    descriptor->version[0] = '1'; descriptor->version[1] = '.';
    descriptor->version[2] = '0';
    descriptor->phase_count = 1;
    descriptor->resource_count = 1;
    descriptor->relationship_count = 1;
    return RTFW_STATUS_OK;
}

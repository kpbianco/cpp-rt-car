#include <rt/extension_abi.h>

#include <stddef.h>

_Static_assert(RTFW_EXTENSION_ABI_VERSION == 1u, "extension ABI version");
_Static_assert(sizeof(rtfw_extension_descriptor_v1) == 216,
               "extension descriptor prefix");
_Static_assert(offsetof(rtfw_extension_host_api_v1, stage_phase) == 64,
               "extension host prefix");

int main(void) {
    return RTFW_EXTENSION_PHASE_CAPACITY == 64u &&
                   RTFW_RUNTIME_EXTENSION_CAPACITY == 16u
        ? 0
        : 1;
}

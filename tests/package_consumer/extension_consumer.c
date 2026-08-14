#include <rt/extension_abi.h>

#include <stddef.h>

#define RTFW_CONSUMER_CONCAT_INNER(first, second) first##second
#define RTFW_CONSUMER_CONCAT(first, second) \
    RTFW_CONSUMER_CONCAT_INNER(first, second)
#if defined(_MSC_VER)
#define RTFW_CONSUMER_STATIC_ASSERT(condition, message) \
    typedef char RTFW_CONSUMER_CONCAT( \
        rtfw_consumer_static_assert_, __LINE__)[(condition) ? 1 : -1]
#else
#define RTFW_CONSUMER_STATIC_ASSERT(condition, message) \
    _Static_assert(condition, message)
#endif

RTFW_CONSUMER_STATIC_ASSERT(
    RTFW_EXTENSION_ABI_VERSION == 1u, "extension ABI version");
RTFW_CONSUMER_STATIC_ASSERT(sizeof(rtfw_extension_descriptor_v1) == 216,
               "extension descriptor prefix");
RTFW_CONSUMER_STATIC_ASSERT(
    offsetof(rtfw_extension_host_api_v1, stage_phase) == 64,
               "extension host prefix");

#undef RTFW_CONSUMER_STATIC_ASSERT
#undef RTFW_CONSUMER_CONCAT
#undef RTFW_CONSUMER_CONCAT_INNER

int main(void) {
    return RTFW_EXTENSION_PHASE_CAPACITY == 64u &&
                   RTFW_RUNTIME_EXTENSION_CAPACITY == 16u
        ? 0
        : 1;
}

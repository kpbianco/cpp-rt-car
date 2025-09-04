#include "rt/runtime.hpp"
#include "rt/c_api.h"

namespace rt {

Capabilities query_capabilities() noexcept {
    // In a real system, these would be detected at runtime.
    return {true, true, true};
}

core::seconds tick_duration(core::seconds dt) noexcept {
    return dt; // placeholder passthrough demonstrating strong typing
}

} // namespace rt

extern "C" {

uint32_t rt_version_major(void) { return RT_VERSION_MAJOR; }
uint32_t rt_version_minor(void) { return RT_VERSION_MINOR; }

rt_capabilities_c rt_query_capabilities(void) {
    auto caps = rt::query_capabilities();
    return {static_cast<uint8_t>(caps.jobs),
            static_cast<uint8_t>(caps.time),
            static_cast<uint8_t>(caps.memory)};
}

} // extern "C"


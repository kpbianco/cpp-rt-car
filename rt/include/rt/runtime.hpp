#pragma once

#include <cstdint>
#include "core/units.hpp"
#include "rt/version.hpp"

namespace rt {

// Compile-time feature flags
// Defaults enable all subsystems; users may disable via template parameters.
template <bool Jobs = true, bool Time = true, bool Memory = true>
struct features {
    static constexpr bool jobs   = Jobs;
    static constexpr bool time   = Time;
    static constexpr bool memory = Memory;
};

// Runtime capability structure
struct Capabilities {
    bool jobs;
    bool time;
    bool memory;
};

// Query runtime capabilities
Capabilities query_capabilities() noexcept;

// Example function using strong types
core::seconds tick_duration(core::seconds dt) noexcept;

} // namespace rt


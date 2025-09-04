#pragma once

// Minimal strong type units used for time and distance.
// In a full implementation, this would leverage mp-units, but here we
// provide a lightweight quantity wrapper to illustrate the concept.

#include <cstdint>

namespace core {

// Generic quantity wrapper enforcing dimensional correctness.
template <typename Unit, typename Rep = double>
struct quantity {
    Rep value;
    constexpr explicit quantity(Rep v) : value(v) {}
    constexpr Rep count() const { return value; }
};

// Unit tags
struct seconds_tag {};
struct meters_tag {};

// Convenient aliases
using seconds = quantity<seconds_tag, double>;
using meters  = quantity<meters_tag, double>;

} // namespace core


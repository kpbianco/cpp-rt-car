#pragma once

#include <cstdint>
#include <rtfw/version.h>

namespace rt {

inline constexpr std::uint32_t version_major = RTFW_VERSION_MAJOR;
inline constexpr std::uint32_t version_minor = RTFW_VERSION_MINOR;
inline constexpr std::uint32_t version_patch = RTFW_VERSION_PATCH;

} // namespace rt

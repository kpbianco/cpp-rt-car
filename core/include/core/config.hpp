#pragma once

#include <cstdint>

namespace core {

// Simple constexpr configuration object with semantic versioning.
struct Config {
    std::uint32_t major{1};
    std::uint32_t minor{0};
    constexpr Config(std::uint32_t maj = 1, std::uint32_t min = 0) : major(maj), minor(min) {}
};

constexpr Config default_config{};

} // namespace core


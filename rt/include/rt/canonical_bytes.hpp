#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace rt {

inline bool store_u32_le(
    std::span<std::byte> output,
    std::size_t offset,
    std::uint32_t value) noexcept {
    if (offset > output.size() || output.size() - offset < sizeof(value)) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>(
            (value >> (index * 8u)) & 0xffu);
    }
    return true;
}

inline bool store_u64_le(
    std::span<std::byte> output,
    std::size_t offset,
    std::uint64_t value) noexcept {
    if (offset > output.size() || output.size() - offset < sizeof(value)) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>(
            (value >> (index * 8u)) & 0xffu);
    }
    return true;
}

inline bool load_u32_le(
    std::span<const std::byte> input,
    std::size_t offset,
    std::uint32_t& value) noexcept {
    value = 0;
    if (offset > input.size() || input.size() - offset < sizeof(value)) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(
                     static_cast<std::uint8_t>(input[offset + index]))
                 << (index * 8u);
    }
    return true;
}

inline bool load_u64_le(
    std::span<const std::byte> input,
    std::size_t offset,
    std::uint64_t& value) noexcept {
    value = 0;
    if (offset > input.size() || input.size() - offset < sizeof(value)) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(
                     static_cast<std::uint8_t>(input[offset + index]))
                 << (index * 8u);
    }
    return true;
}

} // namespace rt

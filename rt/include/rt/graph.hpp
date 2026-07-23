#pragma once

#include <cstdint>
#include <limits>

namespace rt {

inline constexpr std::uint64_t invalid_graph_handle =
    std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint32_t graph_resource_kind_bit =
    std::uint32_t{1} << 31u;
inline constexpr std::uint32_t graph_handle_index_mask =
    graph_resource_kind_bit - 1u;

// Handles are instance-local and remain stable for the lifetime of a Runtime.
// Owner and kind components reject foreign-runtime and cross-kind handles.
struct PhaseHandle {
    std::uint64_t value = invalid_graph_handle;

    constexpr PhaseHandle() noexcept = default;
    explicit constexpr PhaseHandle(std::uint64_t encoded) noexcept
        : value(encoded) {}
    constexpr PhaseHandle(
        std::uint32_t owner,
        std::uint32_t index) noexcept
        : value(
              (static_cast<std::uint64_t>(owner) << 32u) |
              static_cast<std::uint64_t>(index)) {}

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value != invalid_graph_handle &&
               (static_cast<std::uint32_t>(value) &
                graph_resource_kind_bit) == 0;
    }
    [[nodiscard]] constexpr std::uint32_t owner() const noexcept {
        return static_cast<std::uint32_t>(value >> 32u);
    }
    [[nodiscard]] constexpr std::uint32_t index() const noexcept {
        return static_cast<std::uint32_t>(value) &
               graph_handle_index_mask;
    }

    friend constexpr bool operator==(
        PhaseHandle,
        PhaseHandle) noexcept = default;
};

struct ResourceHandle {
    std::uint64_t value = invalid_graph_handle;

    constexpr ResourceHandle() noexcept = default;
    explicit constexpr ResourceHandle(std::uint64_t encoded) noexcept
        : value(encoded) {}
    constexpr ResourceHandle(
        std::uint32_t owner,
        std::uint32_t index) noexcept
        : value(
              (static_cast<std::uint64_t>(owner) << 32u) |
              graph_resource_kind_bit |
              static_cast<std::uint64_t>(index)) {}

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value != invalid_graph_handle &&
               (static_cast<std::uint32_t>(value) &
                graph_resource_kind_bit) != 0;
    }
    [[nodiscard]] constexpr std::uint32_t owner() const noexcept {
        return static_cast<std::uint32_t>(value >> 32u);
    }
    [[nodiscard]] constexpr std::uint32_t index() const noexcept {
        return static_cast<std::uint32_t>(value) &
               graph_handle_index_mask;
    }

    friend constexpr bool operator==(
        ResourceHandle,
        ResourceHandle) noexcept = default;
};

enum class ResourceAccess : std::uint8_t {
    read,
    write,
};

} // namespace rt

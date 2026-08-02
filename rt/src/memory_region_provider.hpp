#pragma once

#include <cstddef>

#include <rt/runtime.hpp>

namespace rt::detail {

class RegionStorage final {
public:
    RegionStorage() = default;
    ~RegionStorage();

    RegionStorage(const RegionStorage&) = delete;
    RegionStorage& operator=(const RegionStorage&) = delete;
    RegionStorage(RegionStorage&& other) noexcept;
    RegionStorage& operator=(RegionStorage&& other) noexcept;

    [[nodiscard]] Status create(
        MemoryRegionProvider& provider,
        MemoryRegionId id,
        std::size_t bytes,
        std::size_t alignment,
        MemoryRegionPolicyReport& report) noexcept;
    [[nodiscard]] Status reset(MemoryRegionPolicyReport* report = nullptr)
        noexcept;

    [[nodiscard]] std::byte* data() noexcept { return allocation_.data; }
    [[nodiscard]] const std::byte* data() const noexcept {
        return allocation_.data;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return allocation_.data_bytes;
    }
    [[nodiscard]] bool owns_allocation() const noexcept {
        return provider_ != nullptr;
    }

private:
    void move_from(RegionStorage& other) noexcept;

    MemoryRegionProvider* provider_ = nullptr;
    MemoryRegionId id_{};
    MemoryRegionAllocation allocation_{};
};

} // namespace rt::detail

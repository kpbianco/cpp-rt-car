#pragma once

#include <rt/runtime.hpp>

namespace rt::detail {

class NativeMemoryRegionProvider final : public MemoryRegionProvider {
public:
    [[nodiscard]] MemoryRegionProviderCapabilities capabilities()
        const noexcept override;
    [[nodiscard]] Status allocate(
        MemoryRegionId id,
        std::size_t payload_bytes,
        std::size_t minimum_alignment,
        const MemoryRegionPolicy& policy,
        MemoryRegionAllocation& allocation,
        int& system_error) noexcept override;
    [[nodiscard]] Status verify(
        MemoryRegionId id,
        const MemoryRegionAllocation& allocation,
        const MemoryRegionPolicy& policy,
        MemoryRegionPolicy& observed,
        int& system_error) noexcept override;
    [[nodiscard]] Status release(
        MemoryRegionId id,
        MemoryRegionAllocation& allocation,
        int& system_error) noexcept override;
};

} // namespace rt::detail

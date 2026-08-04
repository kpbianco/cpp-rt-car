#pragma once

#include "aligned_storage.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <rt/runtime.hpp>

namespace rt::detail {

struct LiveResidentAllocation {
    void* token = nullptr;
    std::uintptr_t allocation_begin = 0;
    std::uintptr_t allocation_end = 0;
    LiveResidentAllocation* next = nullptr;
    bool registered = false;
};

class ResidentRegionSet final {
public:
    ResidentRegionSet(
        const MemoryProvider* provider,
        std::atomic<bool>& callback_active) noexcept;
    ~ResidentRegionSet();

    ResidentRegionSet(const ResidentRegionSet&) = delete;
    ResidentRegionSet& operator=(const ResidentRegionSet&) = delete;
    ResidentRegionSet(ResidentRegionSet&& other) = delete;
    ResidentRegionSet& operator=(ResidentRegionSet&& other) = delete;

    [[nodiscard]] static Status validate_provider(
        const MemoryProvider* provider,
        const char*& diagnostic) noexcept;

    [[nodiscard]] Status acquire(
        CpuMemoryPolicyReport& report,
        const char*& diagnostic) noexcept;
    [[nodiscard]] Status apply_and_verify(
        CpuMemoryPolicyReport& report,
        const char*& diagnostic) noexcept;
    [[nodiscard]] bool rollback(CpuMemoryPolicyReport& report) noexcept;
    void release() noexcept;

    [[nodiscard]] std::span<std::byte> span(
        MemoryRegionId region) noexcept;
    [[nodiscard]] bool provider_backed() const noexcept {
        return provider_ != nullptr;
    }
    [[nodiscard]] bool has_live_tokens() const noexcept;
    [[nodiscard]] bool has_pending_operations() const noexcept;

private:
    struct Region {
        MemoryRegionId id{};
        AlignedStorage owned{};
        void* native_mapping = nullptr;
        std::size_t native_mapping_bytes = 0;
        MemoryProviderAllocation allocation{};
        MemoryProviderObservation applied{};
        LiveResidentAllocation live{};
        bool acquired = false;
        bool operation_applied = false;
        bool native_locked = false;
    };

    [[nodiscard]] Status acquire_one(
        Region& region,
        MemoryPolicyReport& row,
        std::size_t required_alignment,
        const char*& diagnostic) noexcept;
    [[nodiscard]] Status acquire_native(
        Region& region,
        const MemoryProviderAcquireRequest& request,
        bool isolate_for_locking,
        const char*& diagnostic) noexcept;
    [[nodiscard]] Status validate_allocation(
        const Region& region,
        const MemoryProviderAcquireRequest& request,
        const char*& diagnostic) const noexcept;
    enum class RegistrationResult : std::uint8_t {
        registered,
        duplicate_token,
        overlapping_span,
    };
    [[nodiscard]] static RegistrationResult register_live(
        Region& region) noexcept;
    [[nodiscard]] static RegistrationResult register_provisional(
        Region& region) noexcept;
    static void unregister_live(Region& region) noexcept;
    [[nodiscard]] Status apply_one(
        Region& region,
        MemoryPolicyReport& row,
        const char*& diagnostic) noexcept;
    void release_one(Region& region) noexcept;

    [[nodiscard]] MemoryPolicyReport* find_report(
        CpuMemoryPolicyReport& report,
        MemoryRegionId region) noexcept;
    [[nodiscard]] Region* find_region(MemoryRegionId region) noexcept;

    std::array<Region, 3> regions_{};
    MemoryProvider provider_copy_{};
    const MemoryProvider* provider_ = nullptr;
    std::atomic<bool>* callback_active_ = nullptr;
};

} // namespace rt::detail

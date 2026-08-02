#include "memory_region_provider.hpp"

#include <algorithm>
#include <cstdint>

namespace {

bool aligned_pointer(const std::byte* data, std::size_t alignment) noexcept {
    return data == nullptr || alignment == 0 ||
           (reinterpret_cast<std::uintptr_t>(data) & (alignment - 1)) == 0;
}

bool policy_matches(
    const rt::MemoryRegionPolicy& expected,
    const rt::MemoryRegionPolicy& observed) noexcept {
    const auto enabled = rt::MemoryPolicyToggle::enabled;
    return (expected.alignment == 0 || observed.alignment >= expected.alignment) &&
           (expected.page_rounding != enabled ||
            observed.page_rounding == enabled) &&
           (expected.guard_before_bytes == 0 ||
            observed.guard_before_bytes >= expected.guard_before_bytes) &&
           (expected.guard_after_bytes == 0 ||
            observed.guard_after_bytes >= expected.guard_after_bytes) &&
           (expected.prefault != enabled || observed.prefault == enabled) &&
           (expected.locking != enabled || observed.locking == enabled) &&
           (expected.pinning != enabled || observed.pinning == enabled) &&
           (expected.huge_pages != rt::HugePagePolicy::require ||
            observed.huge_pages == rt::HugePagePolicy::require) &&
           (expected.numa_node < 0 || observed.numa_node == expected.numa_node) &&
           (expected.first_touch == rt::FirstTouchPolicy::runtime_default ||
            expected.first_touch == rt::FirstTouchPolicy::disabled ||
            observed.first_touch == expected.first_touch) &&
           (expected.residency_verification != enabled ||
            observed.residency_verification == enabled);
}

} // namespace

namespace rt::detail {

RegionStorage::~RegionStorage() {
    (void)reset();
}

RegionStorage::RegionStorage(RegionStorage&& other) noexcept {
    move_from(other);
}

RegionStorage& RegionStorage::operator=(RegionStorage&& other) noexcept {
    if (this != &other) {
        (void)reset();
        move_from(other);
    }
    return *this;
}

Status RegionStorage::create(
    MemoryRegionProvider& provider,
    MemoryRegionId id,
    std::size_t bytes,
    std::size_t alignment,
    MemoryRegionPolicyReport& report) noexcept {
    if (provider_ != nullptr) {
        return Status::invalid_state;
    }

    auto selected = report.resolved;
    MemoryRegionAllocation allocation{};
    MemoryRegionPolicy observed{};
    int application_error = 0;
    int verification_error = 0;
    Status application_status = Status::ok;
    Status verification_status = Status::ok;

    const auto attempt = [&]() noexcept {
        allocation = {};
        observed = {};
        application_error = 0;
        verification_error = 0;
        application_status = provider.allocate(
            id,
            bytes,
            alignment,
            selected,
            allocation,
            application_error);
        const bool valid =
            application_status == Status::ok &&
            allocation.data_bytes == bytes &&
            (bytes == 0 || allocation.data != nullptr) &&
            aligned_pointer(
                allocation.data,
                std::max(alignment, selected.alignment));
        if (!valid) {
            if (application_status == Status::ok) {
                application_status = Status::internal_error;
            }
            return application_status;
        }
        verification_status = provider.verify(
            id,
            allocation,
            selected,
            observed,
            verification_error);
        if (verification_status == Status::ok &&
            !policy_matches(selected, observed)) {
            verification_status = Status::invalid_config;
        }
        return verification_status;
    };

    auto status = attempt();
    if (status != Status::ok) {
        const auto requested_application_status = application_status;
        const auto requested_verification_status = verification_status;
        const auto requested_application_error = application_error;
        const auto requested_verification_error = verification_error;
        if (allocation.allocation_handle != nullptr ||
            allocation.data != nullptr) {
            int ignored = 0;
            (void)provider.release(id, allocation, ignored);
        }
        if (report.resolved.requirement == PolicyRequirement::required) {
            report.application_status = requested_application_status;
            report.verification_status = requested_verification_status;
            report.application_system_error = requested_application_error;
            report.verification_system_error = requested_verification_error;
            report.rolled_back = true;
            return status;
        }

        selected = {};
        selected.provider = MemoryProviderOwnership::runtime;
        status = attempt();
        if (status != Status::ok) {
            if (allocation.allocation_handle != nullptr ||
                allocation.data != nullptr) {
                int ignored = 0;
                (void)provider.release(id, allocation, ignored);
            }
            report.application_status = application_status;
            report.verification_status = verification_status;
            report.application_system_error = application_error;
            report.verification_system_error = verification_error;
            report.rolled_back = true;
            return status;
        }
        report.application = PolicyStageState::portable_fallback;
        report.application_status = requested_application_status;
        report.verification_status = requested_verification_status;
        report.application_system_error = requested_application_error;
        report.verification_system_error = requested_verification_error;
    } else {
        report.application = PolicyStageState::applied;
        report.application_status = Status::ok;
        report.verification_status = Status::ok;
        report.application_system_error = application_error;
        report.verification_system_error = verification_error;
    }

    provider_ = &provider;
    id_ = id;
    allocation_ = allocation;
    report.applied = selected;
    report.verified = observed;
    report.verification = PolicyStageState::verified;
    report.committed_bytes = allocation.committed_bytes;
    report.resident = allocation.resident ||
        observed.residency_verification == MemoryPolicyToggle::enabled;
    report.locked = allocation.locked;
    report.pinned = allocation.pinned;
    report.huge_page_fallback = allocation.huge_page_fallback;
    report.rolled_back = false;
    return Status::ok;
}

Status RegionStorage::reset(MemoryRegionPolicyReport* report) noexcept {
    if (provider_ == nullptr) {
        return Status::ok;
    }
    int system_error = 0;
    auto status = provider_->release(id_, allocation_, system_error);
    if (status == Status::ok) {
        provider_ = nullptr;
        allocation_ = {};
        if (report != nullptr) {
            report->committed_bytes = 0;
        }
    }
    return status;
}

void RegionStorage::move_from(RegionStorage& other) noexcept {
    provider_ = other.provider_;
    id_ = other.id_;
    allocation_ = other.allocation_;
    other.provider_ = nullptr;
    other.allocation_ = {};
}

} // namespace rt::detail

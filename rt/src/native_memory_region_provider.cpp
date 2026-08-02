#include "native_memory_region_provider.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#if defined(__linux__)
#    include <pthread.h>
#    include <sys/mman.h>
#    include <sys/syscall.h>
#    include <unistd.h>
#    if defined(SYS_mbind)
#        include <linux/mempolicy.h>
#    endif
#endif

namespace {

enum class AllocationKind : unsigned char {
    aligned_new,
    mapping,
};

struct NativeAllocation {
    AllocationKind kind = AllocationKind::aligned_new;
    void* base = nullptr;
    std::size_t bytes = 0;
    std::size_t alignment = alignof(std::max_align_t);
    bool locked = false;
};

bool checked_add(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        result = 0;
        return false;
    }
    result = left + right;
    return true;
}

bool align_up(
    std::size_t value,
    std::size_t alignment,
    std::size_t& result) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        result = 0;
        return false;
    }
    std::size_t padded = 0;
    if (!checked_add(value, alignment - 1, padded)) {
        result = 0;
        return false;
    }
    result = padded & ~(alignment - 1);
    return true;
}

std::size_t native_page_bytes() noexcept {
#if defined(__linux__)
    const auto value = ::sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<std::size_t>(value) : 4096;
#else
    return 4096;
#endif
}

bool enabled(rt::MemoryPolicyToggle value) noexcept {
    return value == rt::MemoryPolicyToggle::enabled;
}

void clear_allocation(rt::MemoryRegionAllocation& allocation) noexcept {
    allocation = {};
}

} // namespace

namespace rt::detail {

MemoryRegionProviderCapabilities
NativeMemoryRegionProvider::capabilities() const noexcept {
    MemoryRegionProviderCapabilities result{};
#if defined(__linux__)
    result.custom_thread_stack = true;
    result.minimum_thread_stack_bytes = PTHREAD_STACK_MIN;
    result.page_rounding = true;
    result.guards = true;
    result.prefault = true;
    result.locking = true;
    result.pinning = true;
    result.huge_pages = true;
#    if defined(SYS_mbind)
    result.numa_binding = true;
#    endif
    result.first_touch = true;
    result.residency = true;
#endif
    result.page_bytes = native_page_bytes();
    return result;
}

Status NativeMemoryRegionProvider::allocate(
    MemoryRegionId,
    std::size_t payload_bytes,
    std::size_t minimum_alignment,
    const MemoryRegionPolicy& policy,
    MemoryRegionAllocation& allocation,
    int& system_error) noexcept {
    clear_allocation(allocation);
    system_error = 0;
    const auto page_bytes = native_page_bytes();
    const auto alignment = std::max(
        minimum_alignment,
        policy.alignment == 0 ? minimum_alignment : policy.alignment);
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return Status::invalid_config;
    }

    auto* descriptor = new (std::nothrow) NativeAllocation{};
    if (descriptor == nullptr) {
        system_error = ENOMEM;
        return Status::resource_exhausted;
    }

#if defined(__linux__)
    const bool needs_mapping =
        enabled(policy.page_rounding) ||
        policy.guard_before_bytes != 0 ||
        policy.guard_after_bytes != 0 ||
        enabled(policy.locking) || enabled(policy.pinning) ||
        policy.huge_pages == HugePagePolicy::prefer ||
        policy.huge_pages == HugePagePolicy::require ||
        policy.numa_node >= 0 || enabled(policy.prefault) ||
        policy.first_touch == FirstTouchPolicy::frame_thread ||
        enabled(policy.residency_verification) || alignment > page_bytes;
    if (needs_mapping) {
        std::size_t payload_mapping_bytes = 0;
        std::size_t guard_before = 0;
        std::size_t guard_after = 0;
        if (!align_up(std::max<std::size_t>(payload_bytes, 1), page_bytes,
                      payload_mapping_bytes) ||
            !align_up(policy.guard_before_bytes, page_bytes, guard_before) ||
            !align_up(policy.guard_after_bytes, page_bytes, guard_after)) {
            delete descriptor;
            return Status::invalid_config;
        }

        bool used_huge_pages = false;
        bool huge_fallback = false;
        void* base = MAP_FAILED;
        std::size_t mapping_bytes = 0;
        const bool huge_requested =
            policy.huge_pages == HugePagePolicy::prefer ||
            policy.huge_pages == HugePagePolicy::require;
        if (huge_requested && guard_before == 0 && guard_after == 0 &&
            alignment <= (std::size_t{2} << 20)) {
#    if defined(MAP_HUGETLB)
            const auto huge_alignment = std::size_t{2} << 20;
            if (align_up(payload_mapping_bytes, huge_alignment, mapping_bytes)) {
                base = ::mmap(
                    nullptr,
                    mapping_bytes,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                    -1,
                    0);
                used_huge_pages = base != MAP_FAILED;
            }
#    endif
            if (!used_huge_pages) {
                system_error = errno;
                if (policy.huge_pages == HugePagePolicy::require ||
                    !policy.allow_huge_page_fallback) {
                    delete descriptor;
                    return Status::resource_exhausted;
                }
                huge_fallback = true;
            }
        } else if (huge_requested) {
            if (policy.huge_pages == HugePagePolicy::require ||
                !policy.allow_huge_page_fallback) {
                delete descriptor;
                return Status::invalid_config;
            }
            huge_fallback = true;
        }

        std::byte* data = nullptr;
        if (!used_huge_pages) {
            std::size_t requested_mapping = 0;
            if (!checked_add(guard_before, payload_mapping_bytes,
                             requested_mapping) ||
                !checked_add(requested_mapping, guard_after,
                             requested_mapping) ||
                !checked_add(requested_mapping, alignment, mapping_bytes)) {
                delete descriptor;
                return Status::invalid_config;
            }
            base = ::mmap(
                nullptr,
                mapping_bytes,
                PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS,
                -1,
                0);
            if (base == MAP_FAILED) {
                system_error = errno;
                delete descriptor;
                return Status::resource_exhausted;
            }
            const auto raw = reinterpret_cast<std::uintptr_t>(base);
            std::size_t candidate_input = 0;
            std::size_t candidate = 0;
            if (!checked_add(
                    static_cast<std::size_t>(raw),
                    guard_before,
                    candidate_input) ||
                !align_up(
                    candidate_input,
                    std::max(alignment, page_bytes),
                    candidate)) {
                (void)::munmap(base, mapping_bytes);
                delete descriptor;
                return Status::invalid_config;
            }
            data = reinterpret_cast<std::byte*>(candidate);
            if (guard_before != 0 &&
                ::mprotect(data - guard_before, guard_before, PROT_NONE) != 0) {
                system_error = errno;
                (void)::munmap(base, mapping_bytes);
                delete descriptor;
                return Status::internal_error;
            }
            if (guard_after != 0 &&
                ::mprotect(data + payload_mapping_bytes, guard_after,
                           PROT_NONE) != 0) {
                system_error = errno;
                (void)::munmap(base, mapping_bytes);
                delete descriptor;
                return Status::internal_error;
            }
        } else {
            data = static_cast<std::byte*>(base);
        }

        if (payload_bytes != 0) {
            std::memset(data, 0, payload_bytes);
        }

#    if defined(SYS_mbind)
        if (policy.numa_node >= 0) {
            constexpr auto word_bits = sizeof(unsigned long) * 8;
            if (static_cast<unsigned int>(policy.numa_node) >= word_bits) {
                system_error = EINVAL;
                (void)::munmap(base, mapping_bytes);
                delete descriptor;
                return Status::invalid_config;
            }
            unsigned long mask =
                1UL << static_cast<unsigned int>(policy.numa_node);
            if (::syscall(
                    SYS_mbind,
                    data,
                    payload_mapping_bytes,
                    MPOL_BIND,
                    &mask,
                    word_bits,
                    0UL) != 0) {
                system_error = errno;
                (void)::munmap(base, mapping_bytes);
                delete descriptor;
                return Status::internal_error;
            }
        }
#    endif

        const bool request_lock =
            enabled(policy.locking) || enabled(policy.pinning);
        if (request_lock && payload_mapping_bytes != 0 &&
            ::mlock(data, payload_mapping_bytes) != 0) {
            system_error = errno;
            (void)::munmap(base, mapping_bytes);
            delete descriptor;
            return Status::resource_exhausted;
        }

        descriptor->kind = AllocationKind::mapping;
        descriptor->base = base;
        descriptor->bytes = mapping_bytes;
        descriptor->alignment = std::max(alignment, page_bytes);
        descriptor->locked = request_lock;
        allocation.data = data;
        allocation.data_bytes = payload_bytes;
        allocation.allocation_handle = descriptor;
        allocation.committed_bytes = mapping_bytes;
        allocation.alignment = descriptor->alignment;
        allocation.page_rounded = enabled(policy.page_rounding);
        allocation.guarded = guard_before != 0 || guard_after != 0;
        allocation.prefaulted = enabled(policy.prefault);
        allocation.locked = enabled(policy.locking);
        allocation.pinned = enabled(policy.pinning);
        allocation.huge_pages = used_huge_pages;
        allocation.huge_page_fallback = huge_fallback;
        allocation.numa_bound = policy.numa_node >= 0;
        allocation.first_touched =
            policy.first_touch == FirstTouchPolicy::frame_thread;
        allocation.resident = payload_bytes == 0;
        return Status::ok;
    }
#endif

    if (payload_bytes == 0) {
        descriptor->alignment = alignment;
        allocation.allocation_handle = descriptor;
        allocation.alignment = alignment;
        allocation.resident = true;
        return Status::ok;
    }
    try {
        descriptor->base = ::operator new(
            payload_bytes,
            std::align_val_t(alignment));
    } catch (...) {
        system_error = ENOMEM;
        delete descriptor;
        return Status::resource_exhausted;
    }
    std::memset(descriptor->base, 0, payload_bytes);
    descriptor->kind = AllocationKind::aligned_new;
    descriptor->bytes = payload_bytes;
    descriptor->alignment = alignment;
    allocation.data = static_cast<std::byte*>(descriptor->base);
    allocation.data_bytes = payload_bytes;
    allocation.allocation_handle = descriptor;
    allocation.committed_bytes = payload_bytes;
    allocation.alignment = alignment;
    allocation.resident = true;
    return Status::ok;
}

Status NativeMemoryRegionProvider::verify(
    MemoryRegionId,
    const MemoryRegionAllocation& allocation,
    const MemoryRegionPolicy& policy,
    MemoryRegionPolicy& observed,
    int& system_error) noexcept {
    observed = {};
    observed.provider = MemoryProviderOwnership::runtime;
    observed.alignment = allocation.alignment;
    observed.page_rounding = allocation.page_rounded
        ? MemoryPolicyToggle::enabled
        : MemoryPolicyToggle::disabled;
    if (allocation.guarded) {
        observed.guard_before_bytes = policy.guard_before_bytes;
        observed.guard_after_bytes = policy.guard_after_bytes;
    }
    observed.prefault = allocation.prefaulted
        ? MemoryPolicyToggle::enabled
        : MemoryPolicyToggle::disabled;
    observed.locking = allocation.locked
        ? MemoryPolicyToggle::enabled
        : MemoryPolicyToggle::disabled;
    observed.pinning = allocation.pinned
        ? MemoryPolicyToggle::enabled
        : MemoryPolicyToggle::disabled;
    observed.huge_pages = allocation.huge_pages
        ? policy.huge_pages
        : HugePagePolicy::disabled;
    observed.allow_huge_page_fallback = allocation.huge_page_fallback;
    observed.numa_node = allocation.numa_bound ? policy.numa_node : -1;
    observed.first_touch = allocation.first_touched
        ? policy.first_touch
        : FirstTouchPolicy::disabled;
    observed.rollback = policy.rollback;
    system_error = 0;

#if defined(__linux__)
    if (enabled(policy.residency_verification) &&
        allocation.data_bytes != 0) {
        const auto page_bytes = native_page_bytes();
        std::size_t rounded = 0;
        if (!align_up(allocation.data_bytes, page_bytes, rounded)) {
            return Status::invalid_config;
        }
        const auto page_count = rounded / page_bytes;
        auto states = std::unique_ptr<unsigned char[]>(
            new (std::nothrow) unsigned char[page_count]);
        if (!states) {
            system_error = ENOMEM;
            return Status::resource_exhausted;
        }
        if (::mincore(allocation.data, rounded, states.get()) != 0) {
            system_error = errno;
            return Status::internal_error;
        }
        for (std::size_t page = 0; page < page_count; ++page) {
            if ((states[page] & 1U) == 0) {
                return Status::invalid_config;
            }
        }
        observed.residency_verification = MemoryPolicyToggle::enabled;
    } else {
        observed.residency_verification = MemoryPolicyToggle::disabled;
    }
#else
    observed.residency_verification = MemoryPolicyToggle::disabled;
#endif
    return Status::ok;
}

Status NativeMemoryRegionProvider::release(
    MemoryRegionId,
    MemoryRegionAllocation& allocation,
    int& system_error) noexcept {
    system_error = 0;
    auto* descriptor = static_cast<NativeAllocation*>(
        allocation.allocation_handle);
    if (descriptor == nullptr) {
        clear_allocation(allocation);
        return Status::ok;
    }
    if (descriptor->kind == AllocationKind::mapping) {
#if defined(__linux__)
        if (descriptor->locked && allocation.data != nullptr &&
            allocation.data_bytes != 0) {
            (void)::munlock(allocation.data, allocation.data_bytes);
        }
        if (::munmap(descriptor->base, descriptor->bytes) != 0) {
            system_error = errno;
            return Status::internal_error;
        }
#else
        return Status::internal_error;
#endif
    } else if (descriptor->base != nullptr) {
        ::operator delete(
            descriptor->base,
            std::align_val_t(descriptor->alignment));
    }
    delete descriptor;
    clear_allocation(allocation);
    return Status::ok;
}

} // namespace rt::detail

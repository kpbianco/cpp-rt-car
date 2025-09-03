#pragma once
#include <cstddef>
#include <new>
#include <type_traits>

// Simple aligned allocator for std::vector/etc.
// Alignment must be power of two and >= alignof(std::max_align_t).
template <typename T, std::size_t Alignment>
struct AlignedAllocator {
    static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be power of two");
    using value_type = T;

    AlignedAllocator() noexcept {}
    template <class U>
    AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}

    T* allocate(std::size_t n) {
        return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t(Alignment)));
    }
    void deallocate(T* p, std::size_t) noexcept {
        ::operator delete(p, std::align_val_t(Alignment));
    }

    template <class U>
    struct rebind { using other = AlignedAllocator<U, Alignment>; };
};

// Allocators of different types but same alignment are interchangeable
// (needed for some STL implementations)
template <class T1, class T2, std::size_t A>
inline bool operator==(const AlignedAllocator<T1, A>&, const AlignedAllocator<T2, A>&) { return true; }

template <class T1, class T2, std::size_t A>
inline bool operator!=(const AlignedAllocator<T1, A>&, const AlignedAllocator<T2, A>&) { return false; }

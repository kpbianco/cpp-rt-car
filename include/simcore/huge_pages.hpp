#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#ifdef __linux__
#include <sys/mman.h>
#endif

namespace sim {

inline void* alloc_huge(std::size_t bytes) {
#ifdef __linux__
    void* ptr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
                     -1, 0);
    if (ptr != MAP_FAILED)
        return ptr;
#endif
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 2 * 1024 * 1024, bytes) != 0)
        return nullptr;
#ifdef __linux__
    madvise(ptr, bytes, MADV_HUGEPAGE);
#endif
    return ptr;
}

inline void free_huge(void* ptr, std::size_t bytes) {
#ifdef __linux__
    if (ptr && bytes) {
        if (munmap(ptr, bytes) == 0)
            return;
    }
#endif
    free(ptr);
}

enum class StreamAdvice { Normal, Sequential, Random };

inline void madvise_stream(void* ptr, std::size_t bytes, StreamAdvice adv) {
#ifdef __linux__
    int advice = MADV_NORMAL;
    if (adv == StreamAdvice::Sequential)
        advice = MADV_SEQUENTIAL;
    else if (adv == StreamAdvice::Random)
        advice = MADV_RANDOM;
    madvise(ptr, bytes, advice);
#else
    (void)ptr; (void)bytes; (void)adv;
#endif
}

inline std::size_t page_color(void* ptr, std::size_t pageSize = 4096) {
    std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ptr);
    return (addr / pageSize) & 0x3f; // simple 64-color scheme
}

} // namespace sim


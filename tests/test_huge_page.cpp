#include <gtest/gtest.h>
#include <simcore/huge_pages.hpp>
#include <simcore/prefetch.hpp>

TEST(MemoryDiscipline, HugePagesAndPrefetch) {
    const std::size_t bytes = 2 * 1024 * 1024;
    void* ptr = sim::alloc_huge(bytes);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(ptr) % (2 * 1024 * 1024), 0u);
    sim::madvise_stream(ptr, bytes, sim::StreamAdvice::Sequential);
    auto color = sim::page_color(ptr);
    EXPECT_LT(color, 64u);
    int* iptr = static_cast<int*>(ptr);
    // Prefetch should be a no-op for small working sets and only fire when the
    // range is large enough to benefit. We exercise both paths here.
    sim::prefetch(iptr, 8, 4096, sim::PrefetchMode::Enabled);
    sim::prefetch(iptr, 8, 4096, sim::PrefetchMode::Disabled);
    sim::prefetch(iptr, 8, 4, sim::PrefetchMode::Enabled); // small-N path
    sim::free_huge(ptr, bytes);
}


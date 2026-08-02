#pragma once

#include "memory_region_provider.hpp"

#include <cstddef>
#include <span>
#include <thread>

#if defined(__linux__)
#    include <pthread.h>
#endif

namespace rt::detail {

class OwnedThread final {
public:
    using Entry = void (*)(void*, std::size_t) noexcept;

    OwnedThread() = default;
    ~OwnedThread();
    OwnedThread(const OwnedThread&) = delete;
    OwnedThread& operator=(const OwnedThread&) = delete;
    OwnedThread(OwnedThread&& other) noexcept;
    OwnedThread& operator=(OwnedThread&& other) noexcept;

    [[nodiscard]] Status start(
        MemoryRegionProvider& provider,
        std::span<MemoryRegionPolicyReport> reports,
        ThreadResourceId id,
        Entry entry,
        void* context,
        std::size_t index) noexcept;
    [[nodiscard]] Status start(
        Entry entry,
        void* context,
        std::size_t index) noexcept;
    void join() noexcept;
    [[nodiscard]] bool joinable() const noexcept;

private:
#if defined(__linux__)
    static void* pthread_entry(void* opaque) noexcept;
#endif
    void run() noexcept;
    void move_from(OwnedThread& other) noexcept;

    Entry entry_ = nullptr;
    void* context_ = nullptr;
    std::size_t index_ = 0;
    RegionStorage stack_storage_{};
    MemoryRegionPolicyReport* stack_report_ = nullptr;
    std::thread portable_thread_{};
#if defined(__linux__)
    pthread_t pthread_{};
    bool pthread_joinable_ = false;
#endif
};

} // namespace rt::detail

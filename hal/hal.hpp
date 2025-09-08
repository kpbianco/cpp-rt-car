#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>
#include <cstdlib>

namespace simcore::hal {

// Memory allocation flags (pinned/hugepage currently no-op)
enum class MemFlags : uint32_t {
    none     = 0,
    pinned   = 1u << 0,
    hugepage = 1u << 1,
};

inline void* alloc(std::size_t bytes, MemFlags /*flags*/ = MemFlags::none) {
    constexpr std::size_t align = 64;
    std::size_t size = (bytes + align - 1) / align * align;
    void* ptr = nullptr;
#if defined(_MSC_VER)
    ptr = _aligned_malloc(size, align);
    if (!ptr) throw std::bad_alloc();
#else
    if (posix_memalign(&ptr, align, size) != 0) throw std::bad_alloc();
#endif
    return ptr;
}

inline void free(void* ptr) {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

// Thread helpers
using Thread = std::thread;

template <typename Fn, typename... Args>
Thread create_thread(Fn&& fn, Args&&... args) {
    return Thread(std::forward<Fn>(fn), std::forward<Args>(args)...);
}

inline void join(Thread& t) {
    if (t.joinable()) t.join();
}

// Fence primitive
class Fence {
public:
    Fence()
        : promise_(std::make_shared<std::promise<void>>()),
          future_(promise_->get_future().share()) {}

    void signal() { promise_->set_value(); }

    bool wait_for(std::chrono::milliseconds timeout) {
        return future_.wait_for(timeout) == std::future_status::ready;
    }

    bool is_signaled() const {
        return future_.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::ready;
    }

private:
    std::shared_ptr<std::promise<void>> promise_;
    std::shared_future<void> future_;
};

inline Fence fence_create() { return Fence{}; }

inline void fence_signal(Fence& f) { f.signal(); }

inline bool fence_wait(Fence& f, std::chrono::milliseconds timeout) {
    return f.wait_for(timeout);
}

// Timer utilities
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

inline TimePoint now() { return Clock::now(); }

inline Duration elapsed(TimePoint start, TimePoint end) { return end - start; }

} // namespace simcore::hal


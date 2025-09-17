#pragma once

#include <tuple>
#include <utility>
#include <chrono>
#include <functional>
#ifdef thread
#undef thread
#endif
#include <thread>

#include "hal.hpp"

namespace simcore::hal::gpu {

using Fence = hal::Fence;

struct Buffer {
    void* data;
    std::size_t size;
};

// Submit a kernel to the mock GPU and return a fence that signals on completion.
// Kernel is executed asynchronously on a detached thread.

template <typename Kernel, typename... Args>
Fence submit(Kernel&& kernel, Args&&... args) {
    Fence fence = hal::fence_create();
    auto args_tuple = std::make_tuple(std::forward<Args>(args)...);

    std::thread([
                    func = std::forward<Kernel>(kernel),
                    fence,
                    args_tuple = std::move(args_tuple)
                ]() mutable {
        std::apply(
            [&func](auto&&... unpacked) mutable {
                std::invoke(std::move(func), std::forward<decltype(unpacked)>(unpacked)...);
            },
            std::move(args_tuple));
        hal::fence_signal(fence);
    }).detach();

    return fence;
}

inline bool fence_wait(Fence& f, std::chrono::milliseconds timeout) {
    return hal::fence_wait(f, timeout);
}

} // namespace simcore::hal::gpu


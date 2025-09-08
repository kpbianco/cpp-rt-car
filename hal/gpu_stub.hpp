#pragma once

#include <thread>
#include <tuple>
#include <utility>
#include <chrono>

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
    Fence f = hal::fence_create();
    auto tuple_args = std::make_tuple(std::forward<Args>(args)...);
    std::thread([
                func = std::forward<Kernel>(kernel),
                f,
                args = std::move(tuple_args)
            ]() mutable {
        std::apply(func, args);
        hal::fence_signal(f);
    }).detach();
    return f;
}

inline bool fence_wait(Fence& f, std::chrono::milliseconds timeout) {
    return hal::fence_wait(f, timeout);
}

} // namespace simcore::hal::gpu


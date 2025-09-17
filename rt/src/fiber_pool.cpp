#include <rt/fiber_pool.hpp>

#include <utility>

namespace rt {

namespace {

Task wait_for_fence_task(simcore::hal::Fence fence, FiberPool& pool) {
    co_await FiberPool::FenceAwaiter{fence, pool};
    co_return;
}

} // namespace

void FiberPool::wait_for_fence(simcore::hal::Fence fence) {
    spawn(wait_for_fence_task(std::move(fence), *this));
}

} // namespace rt


#include <rt/cuda_driver.hpp>

#include <chrono>
#include <cstdint>

#include <cuda.h>

namespace {

rt::CudaDriverResult normalize(CUresult result) noexcept {
    switch (result) {
    case CUDA_SUCCESS:
        return rt::CudaDriverResult::success;
    case CUDA_ERROR_NOT_READY:
        return rt::CudaDriverResult::not_ready;
    case CUDA_ERROR_INVALID_VALUE:
    case CUDA_ERROR_INVALID_HANDLE:
        return rt::CudaDriverResult::invalid_value;
    case CUDA_ERROR_OUT_OF_MEMORY:
        return rt::CudaDriverResult::out_of_memory;
    case CUDA_ERROR_DEINITIALIZED:
    case CUDA_ERROR_NO_DEVICE:
    case CUDA_ERROR_INVALID_DEVICE:
    case CUDA_ERROR_INVALID_CONTEXT:
    case CUDA_ERROR_CONTEXT_IS_DESTROYED:
        return rt::CudaDriverResult::context_lost;
    case CUDA_ERROR_LAUNCH_FAILED:
    case CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES:
    case CUDA_ERROR_LAUNCH_TIMEOUT:
    case CUDA_ERROR_ILLEGAL_ADDRESS:
    case CUDA_ERROR_ASSERT:
    case CUDA_ERROR_ECC_UNCORRECTABLE:
        return rt::CudaDriverResult::launch_failure;
    default:
        return rt::CudaDriverResult::error;
    }
}

template <typename Native>
Native pointer_handle(std::uintptr_t handle) noexcept {
    return reinterpret_cast<Native>(handle);
}

rt::CudaDriverResult push_context(
    void*,
    rt::CudaContext context) noexcept {
    return normalize(cuCtxPushCurrent(
        pointer_handle<CUcontext>(context)));
}

rt::CudaDriverResult pop_context(
    void*,
    rt::CudaContext* out_context) noexcept {
    if (!out_context) {
        return rt::CudaDriverResult::invalid_value;
    }
    CUcontext context = nullptr;
    const auto result = normalize(cuCtxPopCurrent(&context));
    if (result == rt::CudaDriverResult::success) {
        *out_context = reinterpret_cast<std::uintptr_t>(context);
    }
    return result;
}

rt::CudaDriverResult event_create(
    void*,
    rt::CudaEvent* out_event) noexcept {
    if (!out_event) {
        return rt::CudaDriverResult::invalid_value;
    }
    CUevent event = nullptr;
    const auto result = normalize(cuEventCreate(
        &event,
        CU_EVENT_DISABLE_TIMING));
    if (result == rt::CudaDriverResult::success) {
        *out_event = reinterpret_cast<std::uintptr_t>(event);
    }
    return result;
}

rt::CudaDriverResult event_destroy(
    void*,
    rt::CudaEvent event) noexcept {
    return normalize(cuEventDestroy(
        pointer_handle<CUevent>(event)));
}

rt::CudaDriverResult event_record(
    void*,
    rt::CudaEvent event,
    rt::CudaStream stream) noexcept {
    return normalize(cuEventRecord(
        pointer_handle<CUevent>(event),
        pointer_handle<CUstream>(stream)));
}

rt::CudaDriverResult event_query(
    void*,
    rt::CudaEvent event) noexcept {
    return normalize(cuEventQuery(
        pointer_handle<CUevent>(event)));
}

rt::CudaDriverResult event_synchronize(
    void*,
    rt::CudaEvent event) noexcept {
    return normalize(cuEventSynchronize(
        pointer_handle<CUevent>(event)));
}

rt::CudaDriverResult stream_synchronize(
    void*,
    rt::CudaStream stream) noexcept {
    return normalize(cuStreamSynchronize(
        pointer_handle<CUstream>(stream)));
}

rt::CudaDriverResult mem_alloc(
    void*,
    std::uint64_t bytes,
    rt::CudaDeviceAddress* out_address) noexcept {
    if (!out_address) {
        return rt::CudaDriverResult::invalid_value;
    }
    CUdeviceptr address = 0;
    const auto result = normalize(cuMemAlloc(&address, bytes));
    if (result == rt::CudaDriverResult::success) {
        *out_address = static_cast<std::uint64_t>(address);
    }
    return result;
}

rt::CudaDriverResult mem_free(
    void*,
    rt::CudaDeviceAddress address) noexcept {
    return normalize(cuMemFree(
        static_cast<CUdeviceptr>(address)));
}

rt::CudaDriverResult host_register(
    void*,
    void* address,
    std::uint64_t bytes) noexcept {
    return normalize(cuMemHostRegister(
        address,
        bytes,
        CU_MEMHOSTREGISTER_PORTABLE));
}

rt::CudaDriverResult host_unregister(
    void*,
    void* address) noexcept {
    return normalize(cuMemHostUnregister(address));
}

rt::CudaDriverResult memcpy_host_to_device_async(
    void*,
    rt::CudaDeviceAddress destination,
    const void* source,
    std::uint64_t bytes,
    rt::CudaStream stream) noexcept {
    return normalize(cuMemcpyHtoDAsync(
        static_cast<CUdeviceptr>(destination),
        source,
        bytes,
        pointer_handle<CUstream>(stream)));
}

rt::CudaDriverResult memcpy_device_to_host_async(
    void*,
    void* destination,
    rt::CudaDeviceAddress source,
    std::uint64_t bytes,
    rt::CudaStream stream) noexcept {
    return normalize(cuMemcpyDtoHAsync(
        destination,
        static_cast<CUdeviceptr>(source),
        bytes,
        pointer_handle<CUstream>(stream)));
}

rt::CudaDriverResult memcpy_device_to_device_async(
    void*,
    rt::CudaDeviceAddress destination,
    rt::CudaDeviceAddress source,
    std::uint64_t bytes,
    rt::CudaStream stream) noexcept {
    return normalize(cuMemcpyDtoDAsync(
        static_cast<CUdeviceptr>(destination),
        static_cast<CUdeviceptr>(source),
        bytes,
        pointer_handle<CUstream>(stream)));
}

rt::CudaDriverResult memset_d8_async(
    void*,
    rt::CudaDeviceAddress destination,
    std::uint8_t value,
    std::uint64_t bytes,
    rt::CudaStream stream) noexcept {
    return normalize(cuMemsetD8Async(
        static_cast<CUdeviceptr>(destination),
        value,
        bytes,
        pointer_handle<CUstream>(stream)));
}

rt::CudaDriverResult launch_kernel(
    void*,
    rt::CudaFunction function,
    std::uint32_t grid_x,
    std::uint32_t grid_y,
    std::uint32_t grid_z,
    std::uint32_t block_x,
    std::uint32_t block_y,
    std::uint32_t block_z,
    std::uint32_t dynamic_shared_bytes,
    rt::CudaStream stream,
    void* const* arguments) noexcept {
    return normalize(cuLaunchKernel(
        pointer_handle<CUfunction>(function),
        grid_x,
        grid_y,
        grid_z,
        block_x,
        block_y,
        block_z,
        dynamic_shared_bytes,
        pointer_handle<CUstream>(stream),
        const_cast<void**>(arguments),
        nullptr));
}

std::uint64_t monotonic_time_ns(void*) noexcept {
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now)
            .count());
}

} // namespace

namespace rt {

CudaDriverApi cuda_driver_api() noexcept {
    CudaDriverApi driver{};
    driver.push_context = &push_context;
    driver.pop_context = &pop_context;
    driver.event_create = &event_create;
    driver.event_destroy = &event_destroy;
    driver.event_record = &event_record;
    driver.event_query = &event_query;
    driver.event_synchronize = &event_synchronize;
    driver.stream_synchronize = &stream_synchronize;
    driver.mem_alloc = &mem_alloc;
    driver.mem_free = &mem_free;
    driver.host_register = &host_register;
    driver.host_unregister = &host_unregister;
    driver.memcpy_host_to_device_async =
        &memcpy_host_to_device_async;
    driver.memcpy_device_to_host_async =
        &memcpy_device_to_host_async;
    driver.memcpy_device_to_device_async =
        &memcpy_device_to_device_async;
    driver.memset_d8_async = &memset_d8_async;
    driver.launch_kernel = &launch_kernel;
    driver.monotonic_time_ns = &monotonic_time_ns;
    return driver;
}

} // namespace rt

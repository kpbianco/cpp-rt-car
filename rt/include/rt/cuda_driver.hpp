#pragma once

#include <rt/cuda_backend.hpp>

namespace rt {

#if defined(RTFW_CUDA_DRIVER_AVAILABLE)
/*
 * Production adapter backed by the CUDA Driver API. The host remains
 * responsible for cuInit(), context/stream creation, module lifetime, and
 * destroying those resources after CudaDeviceBackend::shutdown().
 */
[[nodiscard]] CudaDriverApi cuda_driver_api() noexcept;
#endif

} // namespace rt

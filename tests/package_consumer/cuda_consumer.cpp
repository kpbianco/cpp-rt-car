#include <rt/cuda_backend.hpp>

int main(int argc, char**) {
    if (argc == 999) {
        rt::CudaDriverApi driver;
        rt::CudaBackendConfig config;
        rt::CudaDeviceBackend backend(driver, config);
        return backend.api().abi_version == RTFW_DEVICE_ABI_VERSION ? 0 : 1;
    }
    return rt::cuda_driver_api_version == 1 ? 0 : 2;
}

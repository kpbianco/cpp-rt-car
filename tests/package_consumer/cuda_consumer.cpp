#include <rt/cuda_backend.hpp>

#include <array>

int main(int argc, char**) {
    if (argc == 999) {
        rt::CudaDriverApi driver;
        rt::CudaBackendConfig config;
        rt::CudaDeviceBackend backend(driver, config);
        const rt::DeviceBackendRegistration registration{
            "installed.cuda.v1", backend.api()};
        const auto native = backend.hal_v2_registration("installed.cuda.v2");
        std::array<rt::CudaGraphBufferBinding, 0> bindings{};
        const auto graph_status = backend.register_graph(1u, 1u, bindings);
        return registration.api.abi_version == RTFW_DEVICE_ABI_VERSION &&
                       native.api.api_version == rt::hal_v2_api_version &&
                       graph_status == RTFW_DEVICE_STATUS_OK
            ? 0
            : 1;
    }
    return rt::cuda_driver_api_version == 1 &&
                   rt::cuda_driver_api_version_2 == 2 &&
                   rt::cuda_device_opcode_graph(1u) == 0x4347'0001u
               ? 0
               : 2;
}

#include <rt/xdma_backend.hpp>

int main(int argc, char**) {
    if (argc == 999) {
        rtfw_device_backend_api api{};
        rt::XdmaDriverApi driver;
        rt::XdmaBackendConfig config;
        rt::XdmaDeviceBackend backend(driver, config);
        api = backend.api();
        return api.abi_version == RTFW_DEVICE_ABI_VERSION ? 0 : 1;
    }
    return rt::xdma_driver_api_version == 1 ? 0 : 2;
}

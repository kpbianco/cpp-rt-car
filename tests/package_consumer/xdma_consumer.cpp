#include <rt/device.hpp>
#include <rt/xdma_backend.hpp>

int main(int argc, char**) {
    if (argc == 999) {
        rtfw_device_backend_api api{};
        rt::XdmaDriverApi driver;
        rt::XdmaBackendConfig config;
        rt::XdmaDeviceBackend backend(driver, config);
        api = backend.api();
        const rt::DeviceBackendRegistration registration{
            "installed.xdma.v1", api};
        const auto native = backend.hal_v2_registration("installed.xdma.v2");
        rt::DeviceCommand command;
        const rt::HalV2BufferReference output{};
        const auto encoded = rt::set_xdma_control_read(command, 0u, output);
        return registration.api.abi_version == RTFW_DEVICE_ABI_VERSION &&
                       native.api.api_version == rt::hal_v2_api_version &&
                       encoded
            ? 0
            : 1;
    }
    return rt::xdma_driver_api_version == 1 &&
                   rt::xdma_driver_api_version_2 == 2 &&
                   rt::xdma_user_event_capacity == 16
               ? 0
               : 2;
}

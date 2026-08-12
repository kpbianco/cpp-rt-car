#include <array>
#include <string_view>

#include <rt/xdma_linux.hpp>

int main() {
    constexpr std::array<std::string_view, 1> paths{"/dev/rtfw-package-test"};
    const rt::LinuxXdmaConfig legacy_config{paths, paths};
    rt::LinuxXdmaDriver legacy_driver(legacy_config);
    const auto legacy = legacy_driver.api();
    rt::LinuxXdmaConfig native_config{paths, paths};
    native_config.user_path = paths[0];
    native_config.event_paths = paths;
    rt::LinuxXdmaDriver native_driver(native_config);
    const auto native = native_driver.api();
    return legacy.api_version == rt::xdma_driver_api_version_1 &&
                   native.api_version == rt::xdma_driver_api_version_2 &&
                   native.control_read32 != nullptr &&
                   native.control_write32 != nullptr &&
                   native.wait_user_event != nullptr &&
                   native.request_stop != nullptr
               ? 0
               : 1;
}

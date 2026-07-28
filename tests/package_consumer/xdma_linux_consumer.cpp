#include <array>
#include <string_view>

#include <rt/xdma_linux.hpp>

int main() {
    constexpr std::array<std::string_view, 1> paths{"/dev/rtfw-package-test"};
    const rt::LinuxXdmaConfig config{paths, paths};
    rt::LinuxXdmaDriver driver(config);
    return driver.api().api_version == rt::xdma_driver_api_version ? 0 : 1;
}

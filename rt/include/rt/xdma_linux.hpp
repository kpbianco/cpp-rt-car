#pragma once

#include <memory>
#include <span>
#include <string_view>

#include <rt/xdma_backend.hpp>

namespace rt {

#if defined(RTFW_XDMA_LINUX_AVAILABLE)

struct LinuxXdmaConfig {
    std::span<const std::string_view> h2c_paths{};
    std::span<const std::string_view> c2h_paths{};
};

/*
 * Adapter for Xilinx/dma_ip_drivers XDMA Linux AXI-MM character nodes, such
 * as /dev/xdma0_h2c_0 and /dev/xdma0_c2h_0. File descriptors are opened by
 * initialize(), remain owned by this object, and close at shutdown().
 */
class LinuxXdmaDriver final {
public:
    explicit LinuxXdmaDriver(const LinuxXdmaConfig& config);
    ~LinuxXdmaDriver();

    LinuxXdmaDriver(LinuxXdmaDriver&&) noexcept;
    LinuxXdmaDriver& operator=(LinuxXdmaDriver&&) noexcept;

    LinuxXdmaDriver(const LinuxXdmaDriver&) = delete;
    LinuxXdmaDriver& operator=(const LinuxXdmaDriver&) = delete;

    [[nodiscard]] XdmaDriverApi api() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif

} // namespace rt

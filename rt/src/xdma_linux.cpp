#include <rt/xdma_linux.hpp>

#if defined(RTFW_XDMA_LINUX_AVAILABLE)

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::size_t kMaximumChannelCount = 256;

rt::XdmaDriverResult errno_result(int error) noexcept {
    switch (error) {
    case EINVAL:
    case EOVERFLOW:
        return rt::XdmaDriverResult::invalid_value;
    case ENOMEM:
    case ENOSPC:
        return rt::XdmaDriverResult::resource_exhausted;
    case ETIMEDOUT:
        return rt::XdmaDriverResult::timeout;
    case ENODEV:
    case ENXIO:
    case EBADF:
        return rt::XdmaDriverResult::device_lost;
    case EIO:
        return rt::XdmaDriverResult::reset_required;
    default:
        return rt::XdmaDriverResult::io_error;
    }
}

bool valid_path(std::string_view path) noexcept {
    return !path.empty() &&
           path.front() == '/' &&
           path.size() < 4096 &&
           path.find('\0') == std::string_view::npos;
}

} // namespace

namespace rt {

struct LinuxXdmaDriver::Impl {
    explicit Impl(const LinuxXdmaConfig& config)
        : h2c_count(config.h2c_paths.size()),
          c2h_count(config.c2h_paths.size()),
          h2c_paths(
              h2c_count != 0
              ? std::make_unique<std::string[]>(h2c_count)
              : nullptr),
          c2h_paths(
              c2h_count != 0
              ? std::make_unique<std::string[]>(c2h_count)
              : nullptr),
          h2c_fds(
              h2c_count != 0
              ? std::make_unique<int[]>(h2c_count)
              : nullptr),
          c2h_fds(
              c2h_count != 0
              ? std::make_unique<int[]>(c2h_count)
              : nullptr) {
        if ((h2c_count == 0 && c2h_count == 0) ||
            h2c_count > kMaximumChannelCount ||
            c2h_count > kMaximumChannelCount) {
            throw std::invalid_argument(
                "XDMA Linux adapter requires bounded channel paths");
        }
        for (std::size_t index = 0; index < h2c_count; ++index) {
            if (!valid_path(config.h2c_paths[index])) {
                throw std::invalid_argument("invalid XDMA H2C path");
            }
            h2c_paths[index] = config.h2c_paths[index];
            h2c_fds[index] = -1;
        }
        for (std::size_t index = 0; index < c2h_count; ++index) {
            if (!valid_path(config.c2h_paths[index])) {
                throw std::invalid_argument("invalid XDMA C2H path");
            }
            c2h_paths[index] = config.c2h_paths[index];
            c2h_fds[index] = -1;
        }
    }

    ~Impl() {
        if (initialized.load(std::memory_order_acquire)) {
            (void)shutdown(this);
        }
    }

    static Impl* self(void* user_data) noexcept {
        return static_cast<Impl*>(user_data);
    }

    XdmaDriverResult close_all() noexcept {
        auto result = XdmaDriverResult::success;
        for (std::size_t index = 0; index < h2c_count; ++index) {
            if (h2c_fds[index] >= 0) {
                if (::close(h2c_fds[index]) != 0 &&
                    result == XdmaDriverResult::success) {
                    result = errno_result(errno);
                }
                h2c_fds[index] = -1;
            }
        }
        for (std::size_t index = 0; index < c2h_count; ++index) {
            if (c2h_fds[index] >= 0) {
                if (::close(c2h_fds[index]) != 0 &&
                    result == XdmaDriverResult::success) {
                    result = errno_result(errno);
                }
                c2h_fds[index] = -1;
            }
        }
        return result;
    }

    XdmaDriverResult open_all() noexcept {
        for (std::size_t index = 0; index < h2c_count; ++index) {
            h2c_fds[index] = ::open(
                h2c_paths[index].c_str(),
                O_RDWR | O_CLOEXEC);
            if (h2c_fds[index] < 0) {
                const auto result = errno_result(errno);
                (void)close_all();
                return result;
            }
        }
        for (std::size_t index = 0; index < c2h_count; ++index) {
            c2h_fds[index] = ::open(
                c2h_paths[index].c_str(),
                O_RDWR | O_CLOEXEC);
            if (c2h_fds[index] < 0) {
                const auto result = errno_result(errno);
                (void)close_all();
                return result;
            }
        }
        return XdmaDriverResult::success;
    }

    static XdmaDriverResult initialize(void* user_data) noexcept {
        auto* driver = self(user_data);
        if (!driver) {
            return XdmaDriverResult::invalid_value;
        }
        bool expected = false;
        if (!driver->initialized.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return XdmaDriverResult::invalid_value;
        }
        const auto result = driver->open_all();
        if (result != XdmaDriverResult::success) {
            driver->initialized.store(false, std::memory_order_release);
        }
        return result;
    }

    static XdmaTransferResult transfer(
        void* user_data,
        XdmaDirection direction,
        std::uint32_t channel,
        std::uint64_t device_offset,
        void* host_data,
        std::uint64_t bytes) noexcept {
        auto* driver = self(user_data);
        XdmaTransferResult output{};
        output.result = XdmaDriverResult::invalid_value;
        if (!driver || !host_data || bytes == 0 ||
            !driver->initialized.load(std::memory_order_acquire) ||
            device_offset >
                static_cast<std::uint64_t>(
                    std::numeric_limits<off_t>::max()) ||
            bytes >
                static_cast<std::uint64_t>(
                    std::numeric_limits<off_t>::max()) -
                    device_offset) {
            return output;
        }

        int descriptor = -1;
        if (direction == XdmaDirection::host_to_card) {
            if (channel >= driver->h2c_count) {
                return output;
            }
            descriptor = driver->h2c_fds[channel];
        } else if (direction == XdmaDirection::card_to_host) {
            if (channel >= driver->c2h_count) {
                return output;
            }
            descriptor = driver->c2h_fds[channel];
        } else {
            return output;
        }
        if (descriptor < 0) {
            output.result = XdmaDriverResult::device_lost;
            return output;
        }

        auto* cursor = static_cast<std::byte*>(host_data);
        while (output.bytes_transferred < bytes) {
            const auto remaining = bytes - output.bytes_transferred;
            const auto chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    remaining,
                    static_cast<std::uint64_t>(
                        std::numeric_limits<ssize_t>::max())));
            const auto offset = static_cast<off_t>(
                device_offset + output.bytes_transferred);
            const auto completed =
                direction == XdmaDirection::host_to_card
                ? ::pwrite(
                      descriptor,
                      cursor + output.bytes_transferred,
                      chunk,
                      offset)
                : ::pread(
                      descriptor,
                      cursor + output.bytes_transferred,
                      chunk,
                      offset);
            if (completed > 0) {
                output.bytes_transferred +=
                    static_cast<std::uint64_t>(completed);
                continue;
            }
            if (completed < 0 && errno == EINTR) {
                continue;
            }
            output.result =
                completed == 0
                ? XdmaDriverResult::io_error
                : errno_result(errno);
            return output;
        }
        output.result = XdmaDriverResult::success;
        return output;
    }

    static XdmaDriverResult reset(void* user_data) noexcept {
        auto* driver = self(user_data);
        if (!driver ||
            !driver->initialized.load(std::memory_order_acquire)) {
            return XdmaDriverResult::invalid_value;
        }
        const auto close_result = driver->close_all();
        const auto open_result = driver->open_all();
        if (open_result != XdmaDriverResult::success) {
            driver->initialized.store(false, std::memory_order_release);
            return open_result;
        }
        return close_result;
    }

    static XdmaDriverResult shutdown(void* user_data) noexcept {
        auto* driver = self(user_data);
        if (!driver) {
            return XdmaDriverResult::invalid_value;
        }
        if (!driver->initialized.load(std::memory_order_acquire)) {
            return XdmaDriverResult::success;
        }
        const auto result = driver->close_all();
        if (result == XdmaDriverResult::success) {
            driver->initialized.store(false, std::memory_order_release);
        }
        return result;
    }

    static std::uint64_t monotonic_time_ns(void*) noexcept {
        const auto now =
            std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now)
                .count());
    }

    std::size_t h2c_count = 0;
    std::size_t c2h_count = 0;
    std::unique_ptr<std::string[]> h2c_paths;
    std::unique_ptr<std::string[]> c2h_paths;
    std::unique_ptr<int[]> h2c_fds;
    std::unique_ptr<int[]> c2h_fds;
    std::atomic<bool> initialized{false};
};

LinuxXdmaDriver::LinuxXdmaDriver(const LinuxXdmaConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

LinuxXdmaDriver::~LinuxXdmaDriver() = default;
LinuxXdmaDriver::LinuxXdmaDriver(
    LinuxXdmaDriver&&) noexcept = default;
LinuxXdmaDriver& LinuxXdmaDriver::operator=(
    LinuxXdmaDriver&&) noexcept = default;

XdmaDriverApi LinuxXdmaDriver::api() noexcept {
    XdmaDriverApi driver{};
    driver.user_data = impl_.get();
    driver.initialize = &Impl::initialize;
    driver.transfer = &Impl::transfer;
    driver.reset = &Impl::reset;
    driver.shutdown = &Impl::shutdown;
    driver.monotonic_time_ns = &Impl::monotonic_time_ns;
    return driver;
}

} // namespace rt

#endif

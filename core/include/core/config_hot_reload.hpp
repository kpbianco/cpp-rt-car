#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>
#include <string>
#include <system_error>

#include "core/config.hpp"

namespace core {

namespace detail {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
inline std::shared_ptr<Config>
atomic_load_cfg(const std::shared_ptr<Config> *ptr) noexcept {
  return std::atomic_load(ptr);
}

inline void atomic_store_cfg(std::shared_ptr<Config> *ptr,
                             std::shared_ptr<Config> value) {
  std::atomic_store(ptr, std::move(value));
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
} // namespace detail

// Simple hot-reloading wrapper around the Config structure.
//
// The configuration is stored on disk using a trivial two number format:
//   <major> <minor>
// Each reload validates the major version and atomically swaps in the new
// configuration instance so readers always observe a fully constructed object.
class ConfigHotReloader {
public:
  explicit ConfigHotReloader(std::string path)
      : path_(std::move(path)), config_(std::make_shared<Config>()),
        last_write_() {}

  // Load initial configuration from disk. Returns true on success.
  bool load() {
    Config cfg;
    if (!read_from_disk(cfg, detail::atomic_load_cfg(&config_)->major)) {
      return false;
    }
    detail::atomic_store_cfg(&config_, std::make_shared<Config>(cfg));
    last_write_ = std::filesystem::last_write_time(path_);
    return true;
  }

  // Attempt to hot reload the configuration. If the underlying file has not
  // changed since the last successful load, this is a no-op and returns
  // false. When the file changed, the new configuration is validated and
  // swapped in atomically. Returns true when a new configuration was loaded.
  bool hot_reload() {
    std::error_code ec;
    auto current = std::filesystem::last_write_time(path_, ec);
    if (ec || current == last_write_) {
      return false; // either can't stat or nothing changed
    }
    Config cfg;
    if (!read_from_disk(cfg, detail::atomic_load_cfg(&config_)->major)) {
      return false;
    }
    last_write_ = current;
    detail::atomic_store_cfg(&config_, std::make_shared<Config>(cfg));
    return true;
  }

  // Obtain the current configuration snapshot.
  std::shared_ptr<const Config> get() const noexcept {
    return detail::atomic_load_cfg(&config_);
  }

private:
  bool read_from_disk(Config &out, std::uint32_t expected_major) {
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) {
      return false;
    }
    std::uint32_t maj{};
    std::uint32_t min{};
    if (!(in >> maj >> min)) {
      return false;
    }
    // Simple validation: major version must match previously loaded config.
    if (maj != expected_major) {
      return false;
    }
    out.major = maj;
    out.minor = min;
    return true;
  }

  std::string path_;
  std::shared_ptr<Config> config_;
  std::filesystem::file_time_type last_write_;
};

} // namespace core

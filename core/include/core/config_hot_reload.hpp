#pragma once

#if defined(_MSC_VER)
#define _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
#endif

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include "core/config.hpp"

namespace core {

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
    if (!read_from_disk(cfg, std::atomic_load(&config_)->major)) {
      return false;
    }
    std::atomic_store(&config_, std::make_shared<Config>(cfg));
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
    if (!read_from_disk(cfg, std::atomic_load(&config_)->major)) {
      return false;
    }
    last_write_ = current;
    std::atomic_store(&config_, std::make_shared<Config>(cfg));
    return true;
  }

  // Obtain the current configuration snapshot.
  std::shared_ptr<const Config> get() const noexcept {
    return std::atomic_load(&config_);
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

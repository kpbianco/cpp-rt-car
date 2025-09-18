#include "simcore/platform_init.hpp"

#include "simcore/bintrace.hpp"
#include "simcore/highres_clock.hpp"
#include "simcore/logger.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>

#ifdef __linux__
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

namespace simcore {
namespace {
constexpr std::uint32_t kSuccessBit = 0x80000000u;

void log_crumb(bintrace::Trace *trace, PlatformCrumb crumb, bool success,
               std::uint64_t detail = 0) {
  if (!trace)
    return;
  const std::uint32_t payload =
      (static_cast<std::uint32_t>(crumb) & ~kSuccessBit) |
      (success ? kSuccessBit : 0u);
  trace->log(bintrace::EV_PlatformCrumb, payload, detail);
}

#ifdef __linux__

std::vector<int> parse_cpu_list(const std::string &text) {
  std::vector<int> cpus;
  const char *cur = text.c_str();
  while (*cur) {
    while (*cur == ',' || std::isspace(static_cast<unsigned char>(*cur)))
      ++cur;
    if (!*cur)
      break;
    char *end = nullptr;
    long start = std::strtol(cur, &end, 10);
    if (end == cur) {
      ++cur;
      continue;
    }
    cur = end;
    long finish = start;
    if (*cur == '-') {
      ++cur;
      finish = std::strtol(cur, &end, 10);
      if (end == cur)
        finish = start;
      cur = end;
    }
    if (finish < start)
      std::swap(finish, start);
    for (long v = start; v <= finish; ++v) {
      if (v >= 0 && v <= static_cast<long>(std::numeric_limits<int>::max()))
        cpus.push_back(static_cast<int>(v));
    }
    if (*cur == ',')
      ++cur;
  }
  std::sort(cpus.begin(), cpus.end());
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  return cpus;
}

std::vector<int> read_isolated_cpus(Logger *logger) {
  std::ifstream f("/sys/devices/system/cpu/isolated");
  if (!f.is_open()) {
    LOG_DEBUG(logger,
              "[platform] isolated CPU mask unavailable; skipping affinity gate");
    return {};
  }
  std::string line;
  std::getline(f, line);
  auto cpus = parse_cpu_list(line);
  if (cpus.empty()) {
    LOG_DEBUG(logger, "[platform] no isolated CPUs configured; skipping mask");
  }
  return cpus;
}

bool write_file(const char *path, const char *value, int &errOut) {
  int fd = ::open(path, O_WRONLY);
  if (fd < 0) {
    errOut = errno;
    return false;
  }
  const std::size_t len = std::strlen(value);
  ssize_t wr = ::write(fd, value, len);
  if (wr < 0 || static_cast<std::size_t>(wr) != len) {
    errOut = errno ? errno : EIO;
    ::close(fd);
    return false;
  }
  ::close(fd);
  return true;
}

bool thp_already_disabled(const std::string &contents) {
  return contents.find("[never]") != std::string::npos;
}

bool ensure_thp_disabled(Logger *logger, int &errOut) {
  const char *paths[] = {"/sys/kernel/mm/transparent_hugepage/enabled",
                         "/sys/kernel/mm/transparent_hugepage/defrag"};
  bool touched = false;
  bool ok = true;
  errOut = 0;
  for (const char *path : paths) {
    std::ifstream in(path);
    if (!in.is_open()) {
      LOG_DEBUG(logger, "[platform] THP control '{}' unavailable", path);
      continue;
    }
    touched = true;
    std::string contents((std::istreambuf_iterator<char>(in)), {});
    if (thp_already_disabled(contents))
      continue;
    if (!write_file(path, "never\n", errOut)) {
      ok = false;
    }
  }
  if (!touched)
    return true; // THP controls absent; treat as success.
  return ok;
}

uint64_t read_monotonic_raw() {
  struct timespec ts;
  if (::clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
    struct timespec tp;
    ::clock_gettime(CLOCK_MONOTONIC, &tp);
    return static_cast<uint64_t>(tp.tv_sec) * 1000000000ull + tp.tv_nsec;
  }
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

#endif

} // namespace

PlatformInitState platform_init_rt(const PlatformInitOptions &opts,
                                   Logger *logger, bintrace::Trace *trace) {
  PlatformInitState state;
  state.requested = opts.enable;
  log_crumb(trace, PlatformCrumb::Gate, true, opts.enable ? 1u : 0u);
  if (!opts.enable) {
    LOG_DEBUG(logger, "[platform] real-time gate disabled");
    return state;
  }

  LOG_INFO(logger, "[platform] applying real-time platform policy");

#ifdef __linux__
  if (opts.lockMemory) {
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
      state.lockedMemory = true;
      LOG_INFO(logger, "[platform] mlockall engaged");
      log_crumb(trace, PlatformCrumb::LockMemory, true, 0);
    } else {
      const int err = errno;
      LOG_WARN(logger, "[platform] mlockall failed errno={}", err);
      log_crumb(trace, PlatformCrumb::LockMemory, false,
                static_cast<std::uint64_t>(err));
    }
  }

  if (opts.restrictToIsolatedCpus) {
    auto isolated = read_isolated_cpus(logger);
    if (!isolated.empty()) {
      cpu_set_t mask;
      CPU_ZERO(&mask);
      for (int cpu : isolated) {
        if (cpu >= 0 && cpu < CPU_SETSIZE)
          CPU_SET(static_cast<unsigned>(cpu), &mask);
      }
      if (::sched_setaffinity(0, sizeof(mask), &mask) == 0) {
        state.affinityRestricted = true;
        LOG_INFO(logger, "[platform] restricted affinity to {} isolated cores",
                 isolated.size());
        log_crumb(trace, PlatformCrumb::IsolatedAffinity, true,
                  static_cast<std::uint64_t>(isolated.size()));
      } else {
        const int err = errno;
        LOG_WARN(logger, "[platform] sched_setaffinity failed errno={}", err);
        log_crumb(trace, PlatformCrumb::IsolatedAffinity, false,
                  static_cast<std::uint64_t>(err));
      }
    } else {
      log_crumb(trace, PlatformCrumb::IsolatedAffinity, true, 0);
    }
  }

  if (opts.disableTransparentHugePages) {
    int err = 0;
    bool thpOk = ensure_thp_disabled(logger, err);
    state.transparentHugePagesDisabled = thpOk;
    if (thpOk) {
      LOG_INFO(logger, "[platform] transparent huge pages disabled");
      log_crumb(trace, PlatformCrumb::ThpPolicy, true, 0);
    } else {
      LOG_WARN(logger, "[platform] failed to disable THP errno={}", err);
      log_crumb(trace, PlatformCrumb::ThpPolicy, false,
                static_cast<std::uint64_t>(err));
    }
  }

  if (opts.useMonotonicRawClock) {
#ifdef CLOCK_MONOTONIC_RAW
    struct timespec ts;
    if (::clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0) {
      HighResClock::init(&read_monotonic_raw);
      state.monotonicRawClock = true;
      LOG_INFO(logger, "[platform] CLOCK_MONOTONIC_RAW enabled for timing");
      log_crumb(trace, PlatformCrumb::ClockSource, true, 0);
    } else {
      const int err = errno;
      LOG_WARN(logger, "[platform] CLOCK_MONOTONIC_RAW unavailable errno={}",
               err);
      log_crumb(trace, PlatformCrumb::ClockSource, false,
                static_cast<std::uint64_t>(err));
    }
#else
    log_crumb(trace, PlatformCrumb::ClockSource, false, 0);
#endif
  }

  if (opts.promoteWorkers) {
    int priority = opts.fifoPriority;
    int minPr = ::sched_get_priority_min(SCHED_FIFO);
    int maxPr = ::sched_get_priority_max(SCHED_FIFO);
    if (minPr == -1 || maxPr == -1) {
      state.fifoPriority = 0;
    } else {
      if (minPr > maxPr) {
        std::swap(minPr, maxPr);
      }
      if (priority < minPr)
        priority = minPr;
      if (priority > maxPr)
        priority = maxPr;
      state.fifoPriority = priority;
    }
  }
#else
  LOG_DEBUG(logger,
            "[platform] real-time policy unavailable on this platform");
  log_crumb(trace, PlatformCrumb::LockMemory, false, 0);
  log_crumb(trace, PlatformCrumb::IsolatedAffinity, false, 0);
  log_crumb(trace, PlatformCrumb::ThpPolicy, false, 0);
  log_crumb(trace, PlatformCrumb::ClockSource, false, 0);
#endif

  return state;
}

void platform_apply_worker_policy(const PlatformInitState &state,
                                  Logger *logger, bintrace::Trace *trace) {
#ifdef __linux__
  if (!state.requested || state.fifoPriority <= 0)
    return;
  struct sched_param param;
  std::memset(&param, 0, sizeof(param));
  param.sched_priority = state.fifoPriority;
  if (::sched_setscheduler(0, SCHED_FIFO, &param) == 0) {
    LOG_DEBUG(logger, "[platform] worker promoted to SCHED_FIFO priority {}",
              state.fifoPriority);
    log_crumb(trace, PlatformCrumb::WorkerPolicy, true,
              static_cast<std::uint64_t>(state.fifoPriority));
  } else {
    const int err = errno;
    LOG_WARN(logger, "[platform] sched_setscheduler failed errno={}", err);
    log_crumb(trace, PlatformCrumb::WorkerPolicy, false,
              static_cast<std::uint64_t>(err));
  }
#else
  (void)state;
  (void)logger;
  (void)trace;
#endif
}

} // namespace simcore

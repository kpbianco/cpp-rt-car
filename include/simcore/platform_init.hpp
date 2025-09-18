#pragma once
#include <cstdint>

class Logger;
namespace bintrace {
class Trace;
}

namespace simcore {

enum class PlatformCrumb : std::uint32_t {
  Gate = 0,
  LockMemory = 1,
  IsolatedAffinity = 2,
  ThpPolicy = 3,
  ClockSource = 4,
  WorkerPolicy = 5,
};

struct PlatformInitOptions {
  bool enable = false;
  bool lockMemory = true;
  bool promoteWorkers = true;
  bool restrictToIsolatedCpus = true;
  bool disableTransparentHugePages = true;
  bool useMonotonicRawClock = true;
  int fifoPriority = 1;
};

struct PlatformInitState {
  bool requested = false;
  bool lockedMemory = false;
  bool affinityRestricted = false;
  bool transparentHugePagesDisabled = false;
  bool monotonicRawClock = false;
  int fifoPriority = 0;
};

PlatformInitState platform_init_rt(const PlatformInitOptions &opts,
                                   Logger *logger = nullptr,
                                   bintrace::Trace *trace = nullptr);

void platform_apply_worker_policy(const PlatformInitState &state,
                                  Logger *logger = nullptr,
                                  bintrace::Trace *trace = nullptr);

} // namespace simcore

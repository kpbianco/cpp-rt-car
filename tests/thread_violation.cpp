#include <simcore/SimCore.hpp>

void violate_phase_thread_invariant() {
  std::thread worker([] {});
  worker.join();
}


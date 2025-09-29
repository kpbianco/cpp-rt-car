#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <atomic>
#include <thread>
#include <vector>

#include <rt/fiber_pool.hpp>
#include <simcore/hal.hpp>

using namespace std::chrono_literals;

namespace {

struct WaitState {
  std::atomic<std::size_t> resumed{0};
};

static rt::Task unique_fence_task(rt::FiberPool &pool, simcore::hal::Fence &f,
                                  std::atomic<std::size_t> &duplicates,
                                  std::atomic<std::size_t> &completed,
                                  WaitState &state) {
  co_await rt::FiberPool::FenceAwaiter{f, pool};
  auto previous = state.resumed.fetch_add(1, std::memory_order_relaxed);
  if (previous != 0) {
    duplicates.fetch_add(1, std::memory_order_relaxed);
  }
  completed.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST(FiberPool, FenceStressNoDuplicateResumes) {
  constexpr std::size_t kRounds = 16;
  constexpr std::size_t kFencesPerRound = 64;

  for (int threads : {1, 4}) {
    SimCore::Settings settings;
    settings.threads = static_cast<std::size_t>(threads);
    SimCore sim(settings);
    auto &pool = sim.fiberPool();

    std::atomic<std::size_t> duplicates{0};
    std::atomic<std::size_t> completed{0};

    for (std::size_t round = 0; round < kRounds; ++round) {
      std::vector<simcore::hal::Fence> fences(kFencesPerRound);
      std::vector<WaitState> states(kFencesPerRound);

      for (std::size_t i = 0; i < kFencesPerRound; ++i) {
        pool.spawn(unique_fence_task(pool, fences[i], duplicates, completed,
                                     states[i]));
      }

      std::vector<std::thread> signalers;
      signalers.reserve(kFencesPerRound);
      for (auto &f : fences) {
        signalers.emplace_back([&f]() {
          std::this_thread::sleep_for(1ms);
          simcore::hal::fence_signal(f);
        });
      }

      pool.drain();

      for (auto &t : signalers) {
        if (t.joinable()) {
          t.join();
        }
      }

      for (auto &state : states) {
        EXPECT_EQ(state.resumed.load(std::memory_order_relaxed), 1u)
            << "Fence waiter resumed "
            << state.resumed.load(std::memory_order_relaxed)
            << " times";
      }
    }

    EXPECT_EQ(duplicates.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(completed.load(std::memory_order_relaxed),
              kRounds * kFencesPerRound);
  }
}

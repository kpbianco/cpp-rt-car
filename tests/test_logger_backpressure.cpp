#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <simcore/SimCore.hpp>
#include <simcore/logger.hpp>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

TEST(LoggerBackpressure, FileSinkDropsUnderFloodWithoutBlocking) {
  namespace fs = std::filesystem;
  auto path = fs::temp_directory_path() / "logger_backpressure.log";
  std::error_code ec;
  fs::remove(path, ec);

  auto sink = std::make_shared<Logger::AsyncRingFileSink>(
      path.string(), 8, std::chrono::milliseconds(500));

  Logger log;
  log.addSink(sink);

  constexpr int kMessages = 5000;
  auto future = std::async(std::launch::async, [&] {
    for (int i = 0; i < kMessages; ++i) {
      log.info("msg{}", i);
    }
  });

  EXPECT_EQ(future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  future.get();

  const auto drops = sink->dropped();
  EXPECT_GT(drops, 0u);
  EXPECT_EQ(log.total_dropped(), drops);

  sink.reset();
  fs::remove(path, ec);
}

#if !defined(_WIN32)
TEST(LoggerBackpressure, KernelBypassDropsUnderFloodWithoutBlocking) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds), 0);

  for (int fd : fds) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0);
  }

  auto sink = std::make_shared<Logger::AsyncKernelBypassSink>(
      fds[0], 8, std::chrono::milliseconds(500));

  Logger log;
  log.addSink(sink);

  constexpr int kMessages = 5000;
  auto future = std::async(std::launch::async, [&] {
    for (int i = 0; i < kMessages; ++i) {
      log.info("msg{}", i);
    }
  });

  EXPECT_EQ(future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  future.get();

  const auto drops = sink->dropped();
  EXPECT_GT(drops, 0u);
  EXPECT_EQ(log.total_dropped(), drops);

  sink.reset();
  ::close(fds[0]);
  ::close(fds[1]);
}
#endif

TEST(LoggerBackpressure, LogDropsMetricIncreases) {
  namespace fs = std::filesystem;
  const auto pathA = fs::temp_directory_path() / "logger_backpressure_metrics_a.log";
  const auto pathB = fs::temp_directory_path() / "logger_backpressure_metrics_b.log";

  std::error_code ec;
  fs::remove(pathA, ec);
  fs::remove(pathB, ec);

  {
    auto sinkA = std::make_shared<Logger::AsyncRingFileSink>(
        pathA.string(), 8, std::chrono::milliseconds(500));
    auto sinkB = std::make_shared<Logger::AsyncRingFileSink>(
        pathB.string(), 8, std::chrono::milliseconds(500));

    Logger log;
    log.addSink(sinkA);
    log.addSink(sinkB);

    SimCore::Settings settings;
    settings.hz = 240.0;
    settings.maxFrames = 0;
    settings.threads = 1;
    settings.predictiveEnable = false;
    settings.adaptive = false;

    SimCore sim(settings);
    sim.setLogger(&log);

    constexpr int kMessages = 5000;
    auto future = std::async(std::launch::async, [&] {
      for (int i = 0; i < kMessages; ++i) {
        log.info("msg{}", i);
      }
    });

    EXPECT_EQ(future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    future.get();

    const auto dropsA = sinkA->dropped();
    const auto dropsB = sinkB->dropped();
    const auto totalDrops = log.total_dropped();

    EXPECT_GT(dropsA, 0u);
    EXPECT_GT(dropsB, 0u);
    EXPECT_EQ(totalDrops, dropsA + dropsB);

    metrics::Registry registry;
    sim.setMetrics(&registry);

    const auto snapshot = registry.snapshot();
    const auto loggerIt = snapshot.counters.find("logger.dropped");
    ASSERT_NE(loggerIt, snapshot.counters.end());
    EXPECT_EQ(loggerIt->second, static_cast<std::uint64_t>(totalDrops));

    const auto traceIt = snapshot.counters.find("trace.dropped");
    ASSERT_NE(traceIt, snapshot.counters.end());
    const auto traceDrops = sim.bintrace().dropped();
    EXPECT_EQ(traceIt->second, traceDrops);

    const auto logDropsIt = snapshot.counters.find("log_drops");
    ASSERT_NE(logDropsIt, snapshot.counters.end());
    EXPECT_EQ(logDropsIt->second,
              static_cast<std::uint64_t>(totalDrops) + traceDrops);
    EXPECT_GT(logDropsIt->second, 0u);

    sim.setMetrics(nullptr);
    sim.setLogger(nullptr);
  }

  fs::remove(pathA, ec);
  fs::remove(pathB, ec);
}


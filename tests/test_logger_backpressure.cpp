#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
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

  sink.reset();
  ::close(fds[0]);
  ::close(fds[1]);
}
#endif


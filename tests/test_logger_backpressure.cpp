#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <simcore/logger.hpp>

TEST(LoggerBackpressure, DropsUnderFloodWithoutBlocking) {
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


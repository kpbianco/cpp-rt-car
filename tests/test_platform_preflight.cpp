#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include <rt/runtime.hpp>

namespace {

class PreflightClock final : public rt::RuntimeClock {
public:
    std::uint64_t now_ns() noexcept override {
        return now_++;
    }

    rt::Status sleep_until_ns(
        std::uint64_t absolute_ns) noexcept override {
        now_ = absolute_ns;
        return rt::Status::ok;
    }

    bool supports_absolute_sleep() const noexcept override {
        return true;
    }

private:
    std::uint64_t now_ = 1;
};

class FakePreflight final : public rt::PlatformPreflightProbe {
public:
    void inspect(
        std::size_t planned_runtime_bytes,
        const rt::RuntimeClock&,
        rt::PlatformPreflightReport& report) noexcept override {
        ++calls;
        observed_planned_bytes = planned_runtime_bytes;
        report = {};
        report.mode = rt::PlatformPreflightMode::strict;
        report.passed = should_pass;
        report.check_count = rt::platform_check_capacity;
        for (std::size_t index = 0;
             index < report.check_count;
             ++index) {
            auto& check = report.checks[index];
            check.id = static_cast<rt::PlatformCheckId>(index);
            check.status = rt::PlatformCheckStatus::passed;
            std::snprintf(
                check.message.data(),
                check.message.size(),
                "check %zu passed",
                index);
        }
        if (!should_pass) {
            report.checks[failed_check].status =
                rt::PlatformCheckStatus::failed;
            report.checks[failed_check].system_error = 7;
            std::snprintf(
                report.checks[failed_check].message.data(),
                report.checks[failed_check].message.size(),
                "%s",
                "isolated CPU affinity is required");
        }
        if (duplicate_last_id) {
            report.checks.back().id =
                rt::PlatformCheckId::absolute_monotonic_clock;
        }
    }

    bool should_pass = true;
    bool duplicate_last_id = false;
    std::size_t failed_check = 4;
    std::size_t calls = 0;
    std::size_t observed_planned_bytes = 0;
};

rt::Runtime make_strict_runtime(
    PreflightClock& clock,
    FakePreflight& preflight) {
    rt::Runtime runtime(clock, preflight);
    rt::RuntimeConfig config;
    config.platform_preflight_mode =
        rt::PlatformPreflightMode::strict;
    EXPECT_EQ(runtime.configure(config), rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::ok);
    return runtime;
}

} // namespace

TEST(PlatformPreflight, StrictModePassesOnlyACompleteUniqueReport) {
    PreflightClock clock;
    FakePreflight preflight;
    auto runtime = make_strict_runtime(clock, preflight);
    rt::MemoryPlan plan;
    ASSERT_TRUE(runtime.memory_plan(plan));

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(preflight.calls, 1u);
    EXPECT_EQ(preflight.observed_planned_bytes, plan.planned_bytes);

    rt::PlatformPreflightReport report;
    ASSERT_TRUE(runtime.platform_preflight_report(report));
    EXPECT_TRUE(report.passed);
    EXPECT_EQ(report.mode, rt::PlatformPreflightMode::strict);
    EXPECT_EQ(report.check_count, rt::platform_check_capacity);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(PlatformPreflight, StrictFailureIsReportedBeforeWorkersStart) {
    PreflightClock clock;
    FakePreflight preflight;
    preflight.should_pass = false;
    auto runtime = make_strict_runtime(clock, preflight);

    EXPECT_EQ(
        runtime.start(),
        rt::Status::platform_preflight_failed);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_EQ(runtime.executor_stats().worker_starts, 0u);
    EXPECT_NE(
        runtime.last_error().find("isolated CPU affinity"),
        std::string_view::npos);

    rt::PlatformPreflightReport report;
    ASSERT_TRUE(runtime.platform_preflight_report(report));
    EXPECT_FALSE(report.passed);
    ASSERT_EQ(report.check_count, rt::platform_check_capacity);
    EXPECT_EQ(
        report.checks[preflight.failed_check].status,
        rt::PlatformCheckStatus::failed);
    EXPECT_EQ(
        report.checks[preflight.failed_check].system_error,
        7);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(PlatformPreflight, DuplicatePrerequisiteCannotPassStrictMode) {
    PreflightClock clock;
    FakePreflight preflight;
    preflight.duplicate_last_id = true;
    auto runtime = make_strict_runtime(clock, preflight);

    EXPECT_EQ(
        runtime.start(),
        rt::Status::platform_preflight_failed);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_NE(
        runtime.last_error().find("duplicate prerequisites"),
        std::string_view::npos);
    rt::PlatformPreflightReport report;
    ASSERT_TRUE(runtime.platform_preflight_report(report));
    EXPECT_FALSE(report.passed);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(PlatformPreflight, DisabledModeDoesNotProbeOrMutateTheHost) {
    PreflightClock clock;
    FakePreflight preflight;
    rt::Runtime runtime(clock, preflight);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(preflight.calls, 0u);

    rt::PlatformPreflightReport report;
    ASSERT_TRUE(runtime.platform_preflight_report(report));
    EXPECT_EQ(report.mode, rt::PlatformPreflightMode::disabled);
    EXPECT_TRUE(report.passed);
    EXPECT_EQ(report.check_count, 0u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

#include <rt/mock_device.hpp>
#include <rt/runtime.hpp>

namespace {

constexpr std::uint64_t policy_key(rt::ThreadResourceId id) noexcept {
    return (static_cast<std::uint64_t>(id.role) << 32) | id.instance;
}

std::size_t policy_slot(rt::ThreadResourceId id) noexcept {
    return static_cast<std::size_t>(id.role) * 8 + id.instance;
}

void set_name(rt::ThreadPolicy& policy, const char* name) {
    std::size_t index = 0;
    while (name[index] != '\0') {
        policy.name[index] = name[index];
        ++index;
    }
    policy.name[index] = '\0';
}

class FakeThreadPolicyProvider final : public rt::ThreadPolicyProvider {
public:
    rt::ThreadPolicyProviderCapabilities capabilities() const noexcept override {
        return caps;
    }

    rt::Status apply_current_thread(
        rt::ThreadResourceId id,
        const rt::ThreadPolicy& policy,
        rt::ThreadPolicy& applied,
        int& system_error) noexcept override {
        apply_calls.fetch_add(1, std::memory_order_relaxed);
        if (fail_apply_key.load(std::memory_order_relaxed) == policy_key(id)) {
            applied = {};
            system_error = 777;
            return rt::Status::internal_error;
        }
        applied = policy;
        stored_[policy_slot(id)] = policy;
        system_error = 0;
        return rt::Status::ok;
    }

    rt::Status inspect_current_thread(
        rt::ThreadResourceId id,
        rt::ThreadPolicy& observed,
        int& system_error) noexcept override {
        inspect_calls.fetch_add(1, std::memory_order_relaxed);
        observed = id.role == rt::ThreadRole::frame
            ? frame_observed
            : stored_[policy_slot(id)];
        if (mismatch_key.load(std::memory_order_relaxed) == policy_key(id)) {
            observed.name = {};
            observed.cpu_set = {};
            observed.scheduling_class = rt::SchedulingClass::inherit;
            observed.scheduling_priority = 0;
        }
        system_error = 0;
        return rt::Status::ok;
    }

    std::array<rt::ThreadPolicy, 64> stored_{};
    rt::ThreadPolicyProviderCapabilities caps{
        true, true, true, rt::policy_thread_name_capacity};
    rt::ThreadPolicy frame_observed{};
    std::atomic<std::uint64_t> fail_apply_key{0};
    std::atomic<std::uint64_t> mismatch_key{0};
    std::atomic<std::size_t> apply_calls{0};
    std::atomic<std::size_t> inspect_calls{0};
};

rt::ThreadPolicyReport find_report(
    const rt::Runtime& runtime,
    rt::ThreadRole role,
    std::uint32_t instance) {
    rt::CpuMemoryPolicySummary summary;
    EXPECT_TRUE(runtime.cpu_memory_policy_summary(summary));
    for (std::size_t index = 0; index < summary.thread_count; ++index) {
        rt::ThreadPolicyReport report;
        EXPECT_TRUE(runtime.thread_policy_report_at(index, report));
        if (report.id == rt::ThreadResourceId{role, instance}) {
            return report;
        }
    }
    ADD_FAILURE() << "thread policy report not found";
    return {};
}

rt::ThreadPolicyRequest named_required(
    rt::ThreadRole role,
    std::uint32_t instance,
    const char* name) {
    rt::ThreadPolicyRequest request;
    request.id = {role, instance};
    request.policy.requirement = rt::PolicyRequirement::required;
    set_name(request.policy, name);
    return request;
}

} // namespace

TEST(ThreadPolicy, InjectedProviderCommitsEveryRuntimeOwnedRole) {
    FakeThreadPolicyProvider provider;
    std::array requests{
        named_required(rt::ThreadRole::executor_worker, 0, "worker-0"),
        named_required(rt::ThreadRole::executor_worker, 1, "worker-1"),
        named_required(rt::ThreadRole::watchdog_service, 0, "watchdog"),
        named_required(rt::ThreadRole::device_service, 0, "device"),
    };
    requests[0].policy.wait_strategy = rt::WaitStrategy::spin;
    requests[1].policy.wait_strategy = rt::WaitStrategy::yield;

    rt::MockDeviceBackend mock;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_thread_policy_provider(provider), rt::Status::ok);
    rt::RuntimeConfig config;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    config.watchdog_timeout_ns = 1'000'000;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::DeviceBackendHandle backend;
    ASSERT_EQ(
        runtime.register_device_backend({"thread.mock", mock.api()}, backend),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.set_cpu_memory_policy({requests, {}}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    ASSERT_EQ(runtime.start(), rt::Status::ok) << runtime.last_error();

    EXPECT_EQ(provider.apply_calls.load(), 4u);
    EXPECT_EQ(provider.inspect_calls.load(), 4u);
    for (const auto& request : requests) {
        const auto report = find_report(
            runtime,
            request.id.role,
            request.id.instance);
        EXPECT_EQ(report.ownership, rt::ThreadOwnership::runtime);
        EXPECT_EQ(report.resolution, rt::PolicyStageState::native_resolved);
        EXPECT_EQ(report.application, rt::PolicyStageState::applied);
        EXPECT_EQ(report.verification, rt::PolicyStageState::verified);
        EXPECT_EQ(report.application_status, rt::Status::ok);
        EXPECT_EQ(report.verification_status, rt::Status::ok);
        EXPECT_FALSE(report.rolled_back);
    }
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(ThreadPolicy, RequiredApplyFailureRollsBackAndRetryIsClean) {
    FakeThreadPolicyProvider provider;
    std::array requests{
        named_required(rt::ThreadRole::executor_worker, 0, "worker-0"),
        named_required(rt::ThreadRole::executor_worker, 1, "worker-1"),
    };
    provider.fail_apply_key.store(policy_key(requests[1].id));

    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_thread_policy_provider(provider), rt::Status::ok);
    rt::RuntimeConfig config;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.set_cpu_memory_policy({requests, {}}), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();

    EXPECT_EQ(runtime.start(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    auto first = find_report(runtime, rt::ThreadRole::executor_worker, 0);
    auto second = find_report(runtime, rt::ThreadRole::executor_worker, 1);
    EXPECT_TRUE(first.rolled_back);
    EXPECT_EQ(second.application_status, rt::Status::internal_error);
    EXPECT_EQ(second.application_system_error, 777);

    provider.fail_apply_key.store(0);
    ASSERT_EQ(runtime.start(), rt::Status::ok) << runtime.last_error();
    first = find_report(runtime, rt::ThreadRole::executor_worker, 0);
    second = find_report(runtime, rt::ThreadRole::executor_worker, 1);
    EXPECT_FALSE(first.rolled_back);
    EXPECT_FALSE(second.rolled_back);
    EXPECT_EQ(first.verification, rt::PolicyStageState::verified);
    EXPECT_EQ(second.verification, rt::PolicyStageState::verified);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(ThreadPolicy, RequiredVerificationMismatchAbortsBeforeRunning) {
    FakeThreadPolicyProvider provider;
    auto request = named_required(
        rt::ThreadRole::executor_worker,
        0,
        "worker-0");
    provider.mismatch_key.store(policy_key(request.id));

    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_thread_policy_provider(provider), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_cpu_memory_policy({
            std::span<const rt::ThreadPolicyRequest>(&request, 1),
            {}}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    EXPECT_EQ(runtime.start(), rt::Status::invalid_state);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    const auto report = find_report(
        runtime,
        rt::ThreadRole::executor_worker,
        0);
    EXPECT_EQ(report.application, rt::PolicyStageState::applied);
    EXPECT_EQ(report.verification_status, rt::Status::invalid_state);
    EXPECT_TRUE(report.rolled_back);
}

TEST(ThreadPolicy, DevicePolicyFailureReversesBackendStartupBeforeRetry) {
    FakeThreadPolicyProvider provider;
    auto request = named_required(
        rt::ThreadRole::device_service,
        0,
        "device");
    provider.fail_apply_key.store(policy_key(request.id));

    rt::MockDeviceBackend mock;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_thread_policy_provider(provider), rt::Status::ok);
    rt::DeviceBackendHandle backend;
    ASSERT_EQ(
        runtime.register_device_backend({"rollback.mock", mock.api()}, backend),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.set_cpu_memory_policy({
            std::span<const rt::ThreadPolicyRequest>(&request, 1), {}}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();

    EXPECT_EQ(runtime.start(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    provider.fail_apply_key.store(0);
    ASSERT_EQ(runtime.start(), rt::Status::ok) << runtime.last_error();
    const auto report = find_report(
        runtime,
        rt::ThreadRole::device_service,
        0);
    EXPECT_EQ(report.verification, rt::PolicyStageState::verified);
    EXPECT_FALSE(report.rolled_back);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(ThreadPolicy, BestEffortFailureIsReportedWithoutBlockingStart) {
    FakeThreadPolicyProvider provider;
    rt::ThreadPolicyRequest request;
    request.id = {rt::ThreadRole::executor_worker, 0};
    set_name(request.policy, "best-effort");
    provider.fail_apply_key.store(policy_key(request.id));

    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_thread_policy_provider(provider), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_cpu_memory_policy({
            std::span<const rt::ThreadPolicyRequest>(&request, 1),
            {}}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    ASSERT_EQ(runtime.start(), rt::Status::ok) << runtime.last_error();
    const auto report = find_report(
        runtime,
        rt::ThreadRole::executor_worker,
        0);
    EXPECT_EQ(report.application, rt::PolicyStageState::not_performed);
    EXPECT_EQ(report.application_status, rt::Status::internal_error);
    EXPECT_EQ(report.application_system_error, 777);
    EXPECT_EQ(report.verification_status, rt::Status::invalid_state);
    EXPECT_FALSE(report.rolled_back);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(ThreadPolicy, UnsupportedProviderFallsBackOrFailsClosed) {
    FakeThreadPolicyProvider provider;
    provider.caps = {};
    rt::ThreadPolicyRequest best_effort;
    best_effort.id = {rt::ThreadRole::executor_worker, 0};
    set_name(best_effort.policy, "portable");

    rt::Runtime fallback_runtime;
    ASSERT_EQ(
        fallback_runtime.set_thread_policy_provider(provider),
        rt::Status::ok);
    ASSERT_EQ(
        fallback_runtime.set_cpu_memory_policy({
            std::span<const rt::ThreadPolicyRequest>(&best_effort, 1), {}}),
        rt::Status::ok);
    ASSERT_EQ(fallback_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(fallback_runtime.start(), rt::Status::ok);
    const auto fallback = find_report(
        fallback_runtime,
        rt::ThreadRole::executor_worker,
        0);
    EXPECT_EQ(fallback.resolution, rt::PolicyStageState::portable_fallback);
    EXPECT_EQ(fallback.application, rt::PolicyStageState::not_performed);
    EXPECT_EQ(provider.apply_calls.load(), 0u);
    EXPECT_EQ(fallback_runtime.stop(), rt::Status::ok);

    auto required = best_effort;
    required.policy.requirement = rt::PolicyRequirement::required;
    rt::Runtime strict_runtime;
    ASSERT_EQ(
        strict_runtime.set_thread_policy_provider(provider),
        rt::Status::ok);
    ASSERT_EQ(
        strict_runtime.set_cpu_memory_policy({
            std::span<const rt::ThreadPolicyRequest>(&required, 1), {}}),
        rt::Status::ok);
    EXPECT_EQ(strict_runtime.finalize(), rt::Status::invalid_config);
}

TEST(ThreadPolicy, ExternalOwnersRemainVerifyOnlyAndAreNeverApplied) {
    FakeThreadPolicyProvider provider;
    rt::ThreadPolicyRequest frame;
    frame.id = {rt::ThreadRole::frame, 0};
    frame.policy.requirement = rt::PolicyRequirement::required;
    frame.policy.scheduling_class = rt::SchedulingClass::normal;
    provider.frame_observed.scheduling_class = rt::SchedulingClass::normal;

    rt::ThreadPolicyRequest host_worker;
    host_worker.id = {rt::ThreadRole::executor_worker, 0};
    set_name(host_worker.policy, "host-worker");
    std::array requests{frame, host_worker};

    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_thread_policy_provider(provider), rt::Status::ok);
    rt::RuntimeConfig config;
    config.executor_policy = rt::ExecutorPolicy::host_adapter;
    config.executor_queue_capacity = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::HostExecutorAdapter adapter;
    adapter.worker_count = 1;
    adapter.queue_capacity = 2;
    adapter.submit = [](void*, const rt::HostExecutorJob&) noexcept {
        return rt::Status::queue_full;
    };
    adapter.try_execute_one = [](void*) noexcept { return false; };
    ASSERT_EQ(runtime.set_host_executor(adapter), rt::Status::ok);
    ASSERT_EQ(runtime.set_cpu_memory_policy({requests, {}}), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    ASSERT_EQ(runtime.start(), rt::Status::ok) << runtime.last_error();

    const auto frame_report =
        find_report(runtime, rt::ThreadRole::frame, 0);
    const auto worker_report =
        find_report(runtime, rt::ThreadRole::executor_worker, 0);
    EXPECT_EQ(frame_report.ownership, rt::ThreadOwnership::host);
    EXPECT_EQ(frame_report.application, rt::PolicyStageState::not_performed);
    EXPECT_EQ(frame_report.verification, rt::PolicyStageState::verified);
    EXPECT_EQ(worker_report.ownership, rt::ThreadOwnership::host);
    EXPECT_EQ(worker_report.resolution, rt::PolicyStageState::portable_fallback);
    EXPECT_EQ(worker_report.verification, rt::PolicyStageState::verify_only);
    EXPECT_EQ(provider.apply_calls.load(), 0u);
    EXPECT_EQ(provider.inspect_calls.load(), 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(ThreadPolicy, TwoRuntimesUseIndependentProvidersAndReports) {
    FakeThreadPolicyProvider first_provider;
    FakeThreadPolicyProvider second_provider;
    auto first_request = named_required(
        rt::ThreadRole::executor_worker,
        0,
        "first");
    auto second_request = named_required(
        rt::ThreadRole::executor_worker,
        0,
        "second");

    rt::Runtime first;
    rt::Runtime second;
    ASSERT_EQ(first.set_thread_policy_provider(first_provider), rt::Status::ok);
    ASSERT_EQ(second.set_thread_policy_provider(second_provider), rt::Status::ok);
    ASSERT_EQ(
        first.set_cpu_memory_policy({
            std::span<const rt::ThreadPolicyRequest>(&first_request, 1), {}}),
        rt::Status::ok);
    ASSERT_EQ(
        second.set_cpu_memory_policy({
            std::span<const rt::ThreadPolicyRequest>(&second_request, 1), {}}),
        rt::Status::ok);
    ASSERT_EQ(first.finalize(), rt::Status::ok);
    ASSERT_EQ(second.finalize(), rt::Status::ok);
    ASSERT_EQ(first.start(), rt::Status::ok);
    ASSERT_EQ(second.start(), rt::Status::ok);

    const auto first_report = find_report(
        first, rt::ThreadRole::executor_worker, 0);
    const auto second_report = find_report(
        second, rt::ThreadRole::executor_worker, 0);
    EXPECT_NE(first_report.verified.name, second_report.verified.name);
    EXPECT_EQ(first_provider.apply_calls.load(), 1u);
    EXPECT_EQ(second_provider.apply_calls.load(), 1u);
    EXPECT_EQ(first.stop(), rt::Status::ok);
    EXPECT_EQ(second.stop(), rt::Status::ok);
}

#if defined(__linux__)
TEST(ThreadPolicy, LinuxAppliesAndReadsBackUnprivilegedPolicy) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    ASSERT_EQ(::sched_getaffinity(0, sizeof(allowed), &allowed), 0);
    int selected_cpu = -1;
    for (std::size_t cpu = 0; cpu < rt::policy_cpu_capacity; ++cpu) {
        if (CPU_ISSET(static_cast<int>(cpu), &allowed)) {
            selected_cpu = static_cast<int>(cpu);
            break;
        }
    }
    if (selected_cpu < 0) {
        GTEST_SKIP() << "no allowed CPU is representable by the policy model";
    }

    rt::ThreadPolicyRequest request;
    request.id = {rt::ThreadRole::executor_worker, 0};
    request.policy.requirement = rt::PolicyRequirement::required;
    request.policy.cpu_set.specified = true;
    request.policy.cpu_set.logical_cpu_count =
        static_cast<std::uint16_t>(selected_cpu + 1);
    request.policy.cpu_set.words[static_cast<std::size_t>(selected_cpu) / 64] =
        std::uint64_t{1} << (static_cast<std::size_t>(selected_cpu) % 64);
    request.policy.scheduling_class = rt::SchedulingClass::normal;
    set_name(request.policy, "rtfw-m15-w0");

    rt::Runtime runtime;
    ASSERT_EQ(
        runtime.set_cpu_memory_policy({
            std::span<const rt::ThreadPolicyRequest>(&request, 1), {}}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    ASSERT_EQ(runtime.start(), rt::Status::ok) << runtime.last_error();
    const auto report = find_report(
        runtime,
        rt::ThreadRole::executor_worker,
        0);
    EXPECT_EQ(report.application, rt::PolicyStageState::applied);
    EXPECT_EQ(report.verification, rt::PolicyStageState::verified);
    EXPECT_EQ(report.applied.cpu_set.words, request.policy.cpu_set.words);
    EXPECT_EQ(report.verified.cpu_set.words, request.policy.cpu_set.words);
    EXPECT_EQ(report.verified.scheduling_class, rt::SchedulingClass::normal);
    EXPECT_EQ(report.verified.name, request.policy.name);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}
#endif

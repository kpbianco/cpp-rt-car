#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

#include <rt/mock_device.hpp>
#include <rt/runtime.hpp>

namespace {

const rt::ThreadPolicyReport* find_thread(
    const std::vector<rt::ThreadPolicyReport>& reports,
    rt::ThreadRole role,
    std::uint32_t instance) {
    const auto found = std::find_if(
        reports.begin(),
        reports.end(),
        [&](const auto& report) {
            return report.id == rt::ThreadResourceId{role, instance};
        });
    return found == reports.end() ? nullptr : &*found;
}

const rt::MemoryRegionPolicyReport* find_region(
    const std::vector<rt::MemoryRegionPolicyReport>& reports,
    rt::MemoryCategory category,
    std::uint32_t instance = 0,
    rt::ThreadRole role = rt::ThreadRole::none) {
    const auto found = std::find_if(
        reports.begin(),
        reports.end(),
        [&](const auto& report) {
            return report.id ==
                rt::MemoryRegionId{category, role, instance};
        });
    return found == reports.end() ? nullptr : &*found;
}

std::vector<rt::ThreadPolicyReport> thread_reports(
    const rt::Runtime& runtime,
    std::size_t count) {
    std::vector<rt::ThreadPolicyReport> reports;
    for (std::size_t index = 0; index < count; ++index) {
        rt::ThreadPolicyReport report;
        EXPECT_TRUE(runtime.thread_policy_report_at(index, report));
        reports.push_back(report);
    }
    rt::ThreadPolicyReport extra;
    EXPECT_FALSE(runtime.thread_policy_report_at(count, extra));
    return reports;
}

std::vector<rt::MemoryRegionPolicyReport> memory_reports(
    const rt::Runtime& runtime,
    std::size_t count) {
    std::vector<rt::MemoryRegionPolicyReport> reports;
    for (std::size_t index = 0; index < count; ++index) {
        rt::MemoryRegionPolicyReport report;
        EXPECT_TRUE(runtime.memory_policy_report_at(index, report));
        reports.push_back(report);
    }
    rt::MemoryRegionPolicyReport extra;
    EXPECT_FALSE(runtime.memory_policy_report_at(count, extra));
    return reports;
}

rt::Status rejecting_submit(
    void*,
    const rt::HostExecutorJob&) noexcept {
    return rt::Status::queue_full;
}

bool execute_none(void*) noexcept {
    return false;
}

} // namespace

TEST(CpuMemoryPolicy, PortableDefaultsInventoryEveryCurrentResourceOnce) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    config.watchdog_timeout_ns = 1'000;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();

    rt::CpuMemoryPolicySummary summary;
    ASSERT_TRUE(runtime.cpu_memory_policy_summary(summary));
    EXPECT_EQ(summary.thread_count, 4u);
    EXPECT_EQ(summary.runtime_owned_thread_count, 3u);
    EXPECT_EQ(summary.externally_owned_thread_count, 1u);
    EXPECT_EQ(summary.memory_region_count, 10u);

    rt::MemoryPlan plan;
    ASSERT_TRUE(runtime.memory_plan(plan));
    EXPECT_EQ(summary.runtime_accounted_bytes, plan.planned_bytes);

    const auto threads = thread_reports(runtime, summary.thread_count);
    const auto memory = memory_reports(runtime, summary.memory_region_count);
    const auto* frame = find_thread(threads, rt::ThreadRole::frame, 0);
    const auto* worker0 =
        find_thread(threads, rt::ThreadRole::executor_worker, 0);
    const auto* worker1 =
        find_thread(threads, rt::ThreadRole::executor_worker, 1);
    const auto* watchdog =
        find_thread(threads, rt::ThreadRole::watchdog_service, 0);
    ASSERT_NE(frame, nullptr);
    ASSERT_NE(worker0, nullptr);
    ASSERT_NE(worker1, nullptr);
    ASSERT_NE(watchdog, nullptr);
    EXPECT_EQ(frame->ownership, rt::ThreadOwnership::host);
    EXPECT_EQ(frame->verification, rt::PolicyStageState::verify_only);
    EXPECT_EQ(worker0->ownership, rt::ThreadOwnership::runtime);
    EXPECT_EQ(worker0->application, rt::PolicyStageState::not_performed);
    EXPECT_EQ(worker1->resolution, rt::PolicyStageState::portable_default);
    EXPECT_EQ(watchdog->ownership, rt::ThreadOwnership::runtime);

    std::set<std::uint64_t> accounting_keys;
    for (const auto& report : threads) {
        EXPECT_TRUE(accounting_keys.insert(report.accounting_key).second);
    }
    std::size_t accounted = 0;
    for (const auto& report : memory) {
        EXPECT_TRUE(accounting_keys.insert(report.accounting_key).second);
        if (report.accounting_scope ==
            rt::MemoryAccountingScope::runtime_plan) {
            accounted += report.accounted_bytes;
        }
    }
    EXPECT_EQ(accounted, plan.planned_bytes);
    EXPECT_NE(
        find_region(memory, rt::MemoryCategory::runtime_control),
        nullptr);
    EXPECT_NE(
        find_region(
            memory,
            rt::MemoryCategory::thread_stack,
            1,
            rt::ThreadRole::executor_worker),
        nullptr);
}

TEST(CpuMemoryPolicy, HostAdapterWorkersAreExternalVerifyOnly) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.executor_policy = rt::ExecutorPolicy::host_adapter;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    const rt::HostExecutorAdapter adapter{
        nullptr,
        2,
        8,
        &rejecting_submit,
        &execute_none,
    };
    ASSERT_EQ(runtime.set_host_executor(adapter), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();

    rt::CpuMemoryPolicySummary summary;
    ASSERT_TRUE(runtime.cpu_memory_policy_summary(summary));
    EXPECT_EQ(summary.runtime_owned_thread_count, 0u);
    EXPECT_EQ(summary.externally_owned_thread_count, 3u);
    const auto reports = thread_reports(runtime, summary.thread_count);
    for (std::uint32_t index = 0; index < 2; ++index) {
        const auto* worker = find_thread(
            reports,
            rt::ThreadRole::executor_worker,
            index);
        ASSERT_NE(worker, nullptr);
        EXPECT_EQ(worker->ownership, rt::ThreadOwnership::host);
        EXPECT_EQ(worker->verification, rt::PolicyStageState::verify_only);
    }
}

TEST(CpuMemoryPolicy, BestEffortRequestsResolveWithoutNativeApplication) {
    rt::ThreadPolicyRequest thread;
    thread.id = {rt::ThreadRole::xdma_io, 3};
    thread.policy.cpu_set.specified = true;
    thread.policy.cpu_set.logical_cpu_count = 8;
    thread.policy.cpu_set.words[0] = std::uint64_t{1} << 4;
    thread.policy.scheduling_class = rt::SchedulingClass::fifo;
    thread.policy.scheduling_priority = 12;
    thread.policy.numa_node = 1;
    thread.policy.wait_strategy = rt::WaitStrategy::park;
    thread.policy.stack_bytes = 64 * 1024;
    thread.policy.guard_bytes = 4096;
    constexpr char name[] = "rtfw-xdma-3";
    std::copy(std::begin(name), std::end(name), thread.policy.name.begin());

    rt::MemoryPolicyRequest memory;
    memory.id = {
        rt::MemoryCategory::runtime_control,
        rt::ThreadRole::none,
        0};
    memory.policy.provider = rt::MemoryProviderOwnership::runtime;
    memory.policy.alignment = 4096;
    memory.policy.page_rounding = rt::MemoryPolicyToggle::enabled;
    memory.policy.guard_before_bytes = 4096;
    memory.policy.prefault = rt::MemoryPolicyToggle::enabled;
    memory.policy.locking = rt::MemoryPolicyToggle::enabled;
    memory.policy.pinning = rt::MemoryPolicyToggle::enabled;
    memory.policy.huge_pages = rt::HugePagePolicy::prefer;
    memory.policy.allow_huge_page_fallback = true;
    memory.policy.numa_node = 0;
    memory.policy.first_touch = rt::FirstTouchPolicy::frame_thread;
    memory.policy.residency_verification =
        rt::MemoryPolicyToggle::enabled;
    memory.policy.rollback = rt::RollbackIntent::release;

    rt::Runtime runtime;
    ASSERT_EQ(
        runtime.set_cpu_memory_policy({
            std::span<const rt::ThreadPolicyRequest>(&thread, 1),
            std::span<const rt::MemoryPolicyRequest>(&memory, 1)}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();

    rt::CpuMemoryPolicySummary summary;
    ASSERT_TRUE(runtime.cpu_memory_policy_summary(summary));
    const auto threads = thread_reports(runtime, summary.thread_count);
    const auto regions = memory_reports(runtime, summary.memory_region_count);
    const auto* xdma = find_thread(threads, rt::ThreadRole::xdma_io, 3);
    ASSERT_NE(xdma, nullptr);
    EXPECT_EQ(xdma->ownership, rt::ThreadOwnership::backend);
    EXPECT_TRUE(xdma->explicitly_requested);
    EXPECT_EQ(xdma->resolution, rt::PolicyStageState::portable_fallback);
    EXPECT_EQ(xdma->application, rt::PolicyStageState::not_performed);
    EXPECT_EQ(xdma->verification, rt::PolicyStageState::verify_only);
    EXPECT_EQ(xdma->resolved.scheduling_class, rt::SchedulingClass::inherit);

    const auto* control = find_region(
        regions,
        rt::MemoryCategory::runtime_control);
    ASSERT_NE(control, nullptr);
    EXPECT_TRUE(control->explicitly_requested);
    EXPECT_EQ(
        control->resolution,
        rt::PolicyStageState::portable_fallback);
    EXPECT_EQ(control->application, rt::PolicyStageState::not_performed);
    EXPECT_EQ(
        control->resolved.provider,
        rt::MemoryProviderOwnership::runtime);
    EXPECT_GT(control->requested_footprint_bytes, control->reported_bytes);
}

TEST(CpuMemoryPolicy, FinalizationRejectsStrictDuplicateAndContradictoryInputs) {
    {
        rt::ThreadPolicyRequest strict;
        strict.id = {rt::ThreadRole::frame, 0};
        strict.policy.requirement = rt::PolicyRequirement::required;
        strict.policy.stack_bytes = 64 * 1024;
        rt::Runtime runtime;
        ASSERT_EQ(
            runtime.set_cpu_memory_policy({
                std::span<const rt::ThreadPolicyRequest>(&strict, 1),
                {}}),
            rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
        EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    }
    {
        std::array<rt::ThreadPolicyRequest, 2> duplicate{};
        duplicate[0].id = {rt::ThreadRole::frame, 0};
        duplicate[1].id = {rt::ThreadRole::frame, 0};
        rt::Runtime runtime;
        ASSERT_EQ(
            runtime.set_cpu_memory_policy({duplicate, {}}),
            rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    }
    {
        rt::ThreadPolicyRequest contradictory;
        contradictory.id = {rt::ThreadRole::executor_worker, 0};
        contradictory.policy.scheduling_class =
            rt::SchedulingClass::inherit;
        contradictory.policy.scheduling_priority = 1;
        rt::Runtime runtime;
        ASSERT_EQ(
            runtime.set_cpu_memory_policy({
                std::span<const rt::ThreadPolicyRequest>(
                    &contradictory,
                    1),
                {}}),
            rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    }
    {
        rt::ThreadPolicyRequest malformed;
        malformed.id = {rt::ThreadRole::executor_worker, 0};
        malformed.policy.cpu_set.specified = true;
        malformed.policy.cpu_set.logical_cpu_count = 8;
        malformed.policy.cpu_set.words[0] = std::uint64_t{1} << 9;
        rt::Runtime runtime;
        ASSERT_EQ(
            runtime.set_cpu_memory_policy({
                std::span<const rt::ThreadPolicyRequest>(&malformed, 1),
                {}}),
            rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    }
    {
        std::array<rt::MemoryPolicyRequest, 2> duplicate{};
        for (auto& request : duplicate) {
            request.id = {
                rt::MemoryCategory::runtime_control,
                rt::ThreadRole::none,
                0};
        }
        rt::Runtime runtime;
        ASSERT_EQ(
            runtime.set_cpu_memory_policy({{}, duplicate}),
            rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    }
}

TEST(CpuMemoryPolicy, FinalizationRejectsCapacityAndCheckedArithmeticOverflow) {
    {
        std::vector<rt::ThreadPolicyRequest> requests(
            rt::policy_request_capacity + 1);
        for (std::size_t index = 0; index < requests.size(); ++index) {
            requests[index].id = {
                rt::ThreadRole::xdma_io,
                static_cast<std::uint32_t>(index)};
        }
        rt::Runtime runtime;
        ASSERT_EQ(
            runtime.set_cpu_memory_policy({requests, {}}),
            rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    }
    {
        rt::MemoryPolicyRequest request;
        request.id = {
            rt::MemoryCategory::runtime_control,
            rt::ThreadRole::none,
            0};
        request.policy.page_rounding =
            rt::MemoryPolicyToggle::enabled;
        request.policy.guard_before_bytes =
            std::numeric_limits<std::size_t>::max();
        rt::Runtime runtime;
        ASSERT_EQ(
            runtime.set_cpu_memory_policy({
                {},
                std::span<const rt::MemoryPolicyRequest>(&request, 1)}),
            rt::Status::ok);
        EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    }
}

TEST(CpuMemoryPolicy, DeviceAndBorrowedMemoryInventoryIsExact) {
    rt::MockDeviceBackend mock;
    rt::Runtime runtime;
    rt::DeviceBackendHandle backend;
    ASSERT_EQ(
        runtime.register_device_backend({"policy.mock", mock.api()}, backend),
        rt::Status::ok);
    std::array<std::byte, 128> device_buffer{};
    rt::DeviceBufferHandle buffer;
    ASSERT_EQ(
        runtime.register_device_buffer(
            {"policy.buffer", backend, device_buffer},
            buffer),
        rt::Status::ok);
    std::array<std::byte, 32> state{};
    ASSERT_EQ(
        runtime.register_state({"policy.state", 1, state}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();

    rt::CpuMemoryPolicySummary summary;
    ASSERT_TRUE(runtime.cpu_memory_policy_summary(summary));
    EXPECT_EQ(summary.thread_count, 3u);
    EXPECT_EQ(summary.memory_region_count, 12u);
    const auto threads = thread_reports(runtime, summary.thread_count);
    const auto memory = memory_reports(runtime, summary.memory_region_count);
    const auto* service =
        find_thread(threads, rt::ThreadRole::device_service, 0);
    ASSERT_NE(service, nullptr);
    EXPECT_EQ(service->ownership, rt::ThreadOwnership::runtime);

    const auto* backend_storage = find_region(
        memory,
        rt::MemoryCategory::backend_storage);
    const auto* registered_state = find_region(
        memory,
        rt::MemoryCategory::registered_state);
    const auto* registered_buffer = find_region(
        memory,
        rt::MemoryCategory::registered_device_buffer);
    ASSERT_NE(backend_storage, nullptr);
    ASSERT_NE(registered_state, nullptr);
    ASSERT_NE(registered_buffer, nullptr);
    EXPECT_EQ(
        backend_storage->ownership,
        rt::MemoryProviderOwnership::backend);
    EXPECT_EQ(
        registered_state->ownership,
        rt::MemoryProviderOwnership::host);
    EXPECT_EQ(registered_state->reported_bytes, state.size());
    EXPECT_EQ(registered_buffer->reported_bytes, device_buffer.size());
    EXPECT_EQ(registered_buffer->accounted_bytes, 0u);
}

TEST(CpuMemoryPolicy, RuntimeInstancesKeepIndependentResolvedInventories) {
    rt::ThreadPolicyRequest xdma;
    xdma.id = {rt::ThreadRole::xdma_io, 0};

    rt::Runtime first;
    rt::Runtime second;
    rt::RuntimeConfig second_config;
    second_config.worker_count = 2;
    second_config.executor_queue_capacity = 8;
    ASSERT_EQ(second.configure(second_config), rt::Status::ok);
    ASSERT_EQ(
        second.set_cpu_memory_policy({
            std::span<const rt::ThreadPolicyRequest>(&xdma, 1),
            {}}),
        rt::Status::ok);
    ASSERT_EQ(first.finalize(), rt::Status::ok);
    ASSERT_EQ(second.finalize(), rt::Status::ok);

    rt::CpuMemoryPolicySummary first_summary;
    rt::CpuMemoryPolicySummary second_summary;
    ASSERT_TRUE(first.cpu_memory_policy_summary(first_summary));
    ASSERT_TRUE(second.cpu_memory_policy_summary(second_summary));
    EXPECT_EQ(first_summary.thread_count, 2u);
    EXPECT_EQ(second_summary.thread_count, 4u);
    rt::ThreadPolicyReport report;
    EXPECT_FALSE(
        first.thread_policy_report_at(first_summary.thread_count, report));
    const auto second_reports =
        thread_reports(second, second_summary.thread_count);
    EXPECT_NE(
        find_thread(second_reports, rt::ThreadRole::xdma_io, 0),
        nullptr);
}

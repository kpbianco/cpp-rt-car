#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <vector>

#if defined(__linux__)
#    include <pthread.h>
#endif

#include <rt/runtime.hpp>

namespace {

class InjectedMemoryProvider final : public rt::MemoryRegionProvider {
public:
    struct Handle {
        void* allocation = nullptr;
        std::size_t alignment = 0;
    };

    rt::MemoryRegionProviderCapabilities capabilities() const noexcept override {
        rt::MemoryRegionProviderCapabilities result{};
#if defined(__linux__)
        result.custom_thread_stack = true;
        result.minimum_thread_stack_bytes = PTHREAD_STACK_MIN;
#endif
        result.page_rounding = true;
        result.guards = true;
        result.prefault = true;
        result.locking = true;
        result.pinning = true;
        result.huge_pages = true;
        result.numa_binding = true;
        result.first_touch = true;
        result.residency = true;
        result.page_bytes = 4096;
        return result;
    }

    rt::Status allocate(
        rt::MemoryRegionId id,
        std::size_t payload_bytes,
        std::size_t minimum_alignment,
        const rt::MemoryRegionPolicy& policy,
        rt::MemoryRegionAllocation& allocation,
        int& system_error) noexcept override {
        allocation = {};
        system_error = 0;
        allocations.push_back(id);
        if (id.category == fail_category &&
            (!fail_only_nondefault ||
             policy.prefault == rt::MemoryPolicyToggle::enabled)) {
            fail_category = static_cast<rt::MemoryCategory>(0);
            system_error = 73;
            return rt::Status::resource_exhausted;
        }
        const auto alignment = std::max<std::size_t>(
            minimum_alignment,
            policy.alignment == 0 ? minimum_alignment : policy.alignment);
        auto* handle = new (std::nothrow) Handle{};
        if (!handle) {
            return rt::Status::resource_exhausted;
        }
        if (payload_bytes != 0) {
            try {
                handle->allocation = ::operator new(
                    payload_bytes,
                    std::align_val_t(alignment));
            } catch (...) {
                delete handle;
                return rt::Status::resource_exhausted;
            }
        }
        handle->alignment = alignment;
        allocation.data = static_cast<std::byte*>(handle->allocation);
        allocation.data_bytes = payload_bytes;
        allocation.allocation_handle = handle;
        allocation.committed_bytes = payload_bytes;
        allocation.alignment = alignment;
        allocation.page_rounded =
            policy.page_rounding == rt::MemoryPolicyToggle::enabled;
        allocation.guarded = policy.guard_before_bytes != 0 ||
            policy.guard_after_bytes != 0;
        allocation.prefaulted =
            policy.prefault == rt::MemoryPolicyToggle::enabled;
        allocation.locked =
            policy.locking == rt::MemoryPolicyToggle::enabled;
        allocation.pinned =
            policy.pinning == rt::MemoryPolicyToggle::enabled;
        allocation.huge_pages =
            policy.huge_pages == rt::HugePagePolicy::require;
        allocation.numa_bound = policy.numa_node >= 0;
        allocation.first_touched =
            policy.first_touch == rt::FirstTouchPolicy::frame_thread;
        allocation.resident =
            policy.residency_verification ==
            rt::MemoryPolicyToggle::enabled;
        return rt::Status::ok;
    }

    rt::Status verify(
        rt::MemoryRegionId id,
        const rt::MemoryRegionAllocation& allocation,
        const rt::MemoryRegionPolicy& policy,
        rt::MemoryRegionPolicy& observed,
        int& system_error) noexcept override {
        system_error = 0;
        observed = policy;
        observed.provider = rt::MemoryProviderOwnership::runtime;
        observed.alignment = allocation.alignment;
        if (policy.page_rounding == rt::MemoryPolicyToggle::runtime_default) {
            observed.page_rounding = rt::MemoryPolicyToggle::disabled;
        }
        if (policy.prefault == rt::MemoryPolicyToggle::runtime_default) {
            observed.prefault = rt::MemoryPolicyToggle::disabled;
        }
        if (policy.locking == rt::MemoryPolicyToggle::runtime_default) {
            observed.locking = rt::MemoryPolicyToggle::disabled;
        }
        if (policy.pinning == rt::MemoryPolicyToggle::runtime_default) {
            observed.pinning = rt::MemoryPolicyToggle::disabled;
        }
        if (policy.huge_pages == rt::HugePagePolicy::runtime_default) {
            observed.huge_pages = rt::HugePagePolicy::disabled;
        }
        if (policy.first_touch == rt::FirstTouchPolicy::runtime_default) {
            observed.first_touch = rt::FirstTouchPolicy::disabled;
        }
        if (policy.residency_verification ==
            rt::MemoryPolicyToggle::runtime_default) {
            observed.residency_verification =
                rt::MemoryPolicyToggle::disabled;
        }
        if (id.category == mismatch_category) {
            observed.prefault = rt::MemoryPolicyToggle::disabled;
        }
        return rt::Status::ok;
    }

    rt::Status release(
        rt::MemoryRegionId id,
        rt::MemoryRegionAllocation& allocation,
        int& system_error) noexcept override {
        system_error = 0;
        releases.push_back(id);
        auto* handle = static_cast<Handle*>(allocation.allocation_handle);
        if (handle != nullptr) {
            if (handle->allocation != nullptr) {
                ::operator delete(
                    handle->allocation,
                    std::align_val_t(handle->alignment));
            }
            delete handle;
        }
        allocation = {};
        return rt::Status::ok;
    }

    rt::MemoryCategory fail_category = static_cast<rt::MemoryCategory>(0);
    rt::MemoryCategory mismatch_category = static_cast<rt::MemoryCategory>(0);
    bool fail_only_nondefault = false;
    std::vector<rt::MemoryRegionId> allocations;
    std::vector<rt::MemoryRegionId> releases;
};

rt::CallbackResult count_callback(
    void* opaque,
    const rt::CallbackContext&) {
    ++*static_cast<int*>(opaque);
    return rt::CallbackResult::ok;
}

rt::MemoryRegionPolicyReport find_memory_report(
    const rt::Runtime& runtime,
    rt::MemoryCategory category,
    rt::ThreadRole role = rt::ThreadRole::none,
    std::uint32_t instance = 0) {
    rt::CpuMemoryPolicySummary summary{};
    EXPECT_TRUE(runtime.cpu_memory_policy_summary(summary));
    for (std::size_t index = 0; index < summary.memory_region_count; ++index) {
        rt::MemoryRegionPolicyReport report{};
        EXPECT_TRUE(runtime.memory_policy_report_at(index, report));
        if (report.id.category == category &&
            report.id.thread_role == role &&
            report.id.instance == instance) {
            return report;
        }
    }
    ADD_FAILURE() << "memory report not found";
    return {};
}

TEST(MemoryRegionPolicy, InjectedProviderCreatesAndReleasesOwnedRegions) {
    InjectedMemoryProvider provider;
    int callbacks = 0;
    {
        rt::Runtime runtime;
        ASSERT_EQ(runtime.set_memory_region_provider(provider), rt::Status::ok);
        rt::RuntimeConfig config{};
        config.worker_count = 1;
        config.scratch_bytes = 8192;
        config.task_scratch_bytes = 4096;
        config.trace_capacity = 16;
        ASSERT_EQ(runtime.configure(config), rt::Status::ok);
        ASSERT_EQ(
            runtime.register_callback({"phase", &count_callback, &callbacks}),
            rt::Status::ok);

        rt::MemoryPolicyRequest request{};
        request.id = {rt::MemoryCategory::phase_scratch,
                      rt::ThreadRole::none, 0};
        request.policy.requirement = rt::PolicyRequirement::required;
        request.policy.provider = rt::MemoryProviderOwnership::runtime;
        request.policy.alignment = 128;
        request.policy.page_rounding = rt::MemoryPolicyToggle::enabled;
        request.policy.guard_before_bytes = 4096;
        request.policy.guard_after_bytes = 4096;
        request.policy.prefault = rt::MemoryPolicyToggle::enabled;
        request.policy.locking = rt::MemoryPolicyToggle::enabled;
        request.policy.pinning = rt::MemoryPolicyToggle::enabled;
        request.policy.huge_pages = rt::HugePagePolicy::require;
        request.policy.numa_node = 0;
        request.policy.first_touch = rt::FirstTouchPolicy::frame_thread;
        request.policy.residency_verification =
            rt::MemoryPolicyToggle::enabled;
        request.policy.rollback = rt::RollbackIntent::release;
        const std::array requests{request};
        ASSERT_EQ(
            runtime.set_cpu_memory_policy({{}, requests}),
            rt::Status::ok);
        ASSERT_EQ(runtime.finalize(), rt::Status::ok);

        const auto report = find_memory_report(
            runtime, rt::MemoryCategory::phase_scratch);
        EXPECT_EQ(report.resolution, rt::PolicyStageState::native_resolved);
        EXPECT_EQ(report.application, rt::PolicyStageState::applied);
        EXPECT_EQ(report.verification, rt::PolicyStageState::verified);
        EXPECT_EQ(report.application_status, rt::Status::ok);
        EXPECT_EQ(report.verification_status, rt::Status::ok);
        EXPECT_TRUE(report.resident);
        EXPECT_TRUE(report.locked);
        EXPECT_TRUE(report.pinned);

        ASSERT_EQ(runtime.start(), rt::Status::ok);
        ASSERT_EQ(runtime.step({0, std::chrono::nanoseconds{1}, {}}),
                  rt::Status::ok);
        EXPECT_EQ(callbacks, 1);
        ASSERT_EQ(runtime.stop(), rt::Status::ok);
    }

    ASSERT_EQ(provider.allocations.size(), 3u);
    EXPECT_EQ(provider.allocations[0].category,
              rt::MemoryCategory::phase_scratch);
    EXPECT_EQ(provider.allocations[1].category,
              rt::MemoryCategory::task_scratch);
    EXPECT_EQ(provider.allocations[2].category,
              rt::MemoryCategory::trace_storage);
    ASSERT_EQ(provider.releases.size(), 3u);
    EXPECT_EQ(provider.releases[0].category,
              rt::MemoryCategory::trace_storage);
    EXPECT_EQ(provider.releases[1].category,
              rt::MemoryCategory::task_scratch);
    EXPECT_EQ(provider.releases[2].category,
              rt::MemoryCategory::phase_scratch);
}

TEST(MemoryRegionPolicy, StrictFailureRollsBackInReverseAndRetryIsClean) {
    InjectedMemoryProvider provider;
    provider.fail_category = rt::MemoryCategory::trace_storage;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_memory_region_provider(provider), rt::Status::ok);
    ASSERT_EQ(runtime.register_callback({"phase", &count_callback, nullptr}),
              rt::Status::ok);

    rt::MemoryPolicyRequest request{};
    request.id = {rt::MemoryCategory::trace_storage,
                  rt::ThreadRole::none, 0};
    request.policy.requirement = rt::PolicyRequirement::required;
    request.policy.prefault = rt::MemoryPolicyToggle::enabled;
    const std::array requests{request};
    ASSERT_EQ(runtime.set_cpu_memory_policy({{}, requests}), rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::resource_exhausted);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    const auto failed_trace = find_memory_report(
        runtime, rt::MemoryCategory::trace_storage);
    EXPECT_EQ(failed_trace.application_status,
              rt::Status::resource_exhausted);
    EXPECT_EQ(failed_trace.application_system_error, 73);
    EXPECT_TRUE(failed_trace.rolled_back);
    EXPECT_TRUE(find_memory_report(
        runtime, rt::MemoryCategory::phase_scratch).rolled_back);
    EXPECT_TRUE(find_memory_report(
        runtime, rt::MemoryCategory::task_scratch).rolled_back);
    ASSERT_EQ(provider.releases.size(), 2u);
    EXPECT_EQ(provider.releases[0].category,
              rt::MemoryCategory::task_scratch);
    EXPECT_EQ(provider.releases[1].category,
              rt::MemoryCategory::phase_scratch);

    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MemoryRegionPolicy, BestEffortNativeFailureUsesReportedFallback) {
    InjectedMemoryProvider provider;
    provider.fail_category = rt::MemoryCategory::phase_scratch;
    provider.fail_only_nondefault = true;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_memory_region_provider(provider), rt::Status::ok);
    ASSERT_EQ(runtime.register_callback({"phase", &count_callback, nullptr}),
              rt::Status::ok);
    rt::MemoryPolicyRequest request{};
    request.id = {rt::MemoryCategory::phase_scratch,
                  rt::ThreadRole::none, 0};
    request.policy.prefault = rt::MemoryPolicyToggle::enabled;
    const std::array requests{request};
    ASSERT_EQ(runtime.set_cpu_memory_policy({{}, requests}), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    const auto report = find_memory_report(
        runtime, rt::MemoryCategory::phase_scratch);
    EXPECT_EQ(report.application, rt::PolicyStageState::portable_fallback);
    EXPECT_EQ(report.application_status, rt::Status::resource_exhausted);
    EXPECT_EQ(report.application_system_error, 73);
    EXPECT_EQ(report.applied.prefault, rt::MemoryPolicyToggle::runtime_default);
    EXPECT_EQ(report.verification, rt::PolicyStageState::verified);
}

TEST(MemoryRegionPolicy, RequiredVerificationMismatchReleasesAllRegions) {
    InjectedMemoryProvider provider;
    provider.mismatch_category = rt::MemoryCategory::trace_storage;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_memory_region_provider(provider), rt::Status::ok);
    ASSERT_EQ(runtime.register_callback({"phase", &count_callback, nullptr}),
              rt::Status::ok);
    rt::MemoryPolicyRequest request{};
    request.id = {rt::MemoryCategory::trace_storage,
                  rt::ThreadRole::none, 0};
    request.policy.requirement = rt::PolicyRequirement::required;
    request.policy.prefault = rt::MemoryPolicyToggle::enabled;
    const std::array requests{request};
    ASSERT_EQ(runtime.set_cpu_memory_policy({{}, requests}), rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    const auto report = find_memory_report(
        runtime, rt::MemoryCategory::trace_storage);
    EXPECT_EQ(report.verification_status, rt::Status::invalid_config);
    EXPECT_TRUE(report.rolled_back);
    ASSERT_EQ(provider.releases.size(), 3u);
    EXPECT_EQ(provider.releases[0].category,
              rt::MemoryCategory::trace_storage);
    EXPECT_EQ(provider.releases[1].category,
              rt::MemoryCategory::task_scratch);
    EXPECT_EQ(provider.releases[2].category,
              rt::MemoryCategory::phase_scratch);
}

TEST(MemoryRegionPolicy, RuntimeInstancesKeepProviderStateIsolated) {
    InjectedMemoryProvider first_provider;
    InjectedMemoryProvider second_provider;
    {
        rt::Runtime first;
        rt::Runtime second;
        ASSERT_EQ(first.set_memory_region_provider(first_provider), rt::Status::ok);
        ASSERT_EQ(second.set_memory_region_provider(second_provider), rt::Status::ok);
        ASSERT_EQ(first.register_callback({"first", &count_callback, nullptr}),
                  rt::Status::ok);
        ASSERT_EQ(second.register_callback({"second", &count_callback, nullptr}),
                  rt::Status::ok);
        ASSERT_EQ(first.finalize(), rt::Status::ok);
        ASSERT_EQ(second.finalize(), rt::Status::ok);
        EXPECT_EQ(first_provider.allocations.size(), 3u);
        EXPECT_EQ(second_provider.allocations.size(), 3u);
        EXPECT_TRUE(first_provider.releases.empty());
        EXPECT_TRUE(second_provider.releases.empty());
    }
    EXPECT_EQ(first_provider.releases.size(), 3u);
    EXPECT_EQ(second_provider.releases.size(), 3u);
}

#if defined(__linux__)
TEST(MemoryRegionPolicy, LinuxAppliesGuardPrefaultAndResidencyUnprivileged) {
    rt::Runtime runtime;
    ASSERT_EQ(runtime.register_callback({"phase", &count_callback, nullptr}),
              rt::Status::ok);
    rt::MemoryPolicyRequest request{};
    request.id = {rt::MemoryCategory::phase_scratch,
                  rt::ThreadRole::none, 0};
    request.policy.requirement = rt::PolicyRequirement::required;
    request.policy.alignment = 4096;
    request.policy.page_rounding = rt::MemoryPolicyToggle::enabled;
    request.policy.guard_before_bytes = 4096;
    request.policy.guard_after_bytes = 4096;
    request.policy.prefault = rt::MemoryPolicyToggle::enabled;
    request.policy.first_touch = rt::FirstTouchPolicy::frame_thread;
    request.policy.residency_verification =
        rt::MemoryPolicyToggle::enabled;
    request.policy.rollback = rt::RollbackIntent::release;
    const std::array requests{request};
    ASSERT_EQ(runtime.set_cpu_memory_policy({{}, requests}), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    const auto report = find_memory_report(
        runtime, rt::MemoryCategory::phase_scratch);
    EXPECT_EQ(report.application, rt::PolicyStageState::applied);
    EXPECT_EQ(report.verification, rt::PolicyStageState::verified);
    EXPECT_TRUE(report.resident);
    EXPECT_GT(report.committed_bytes, report.reported_bytes);
}

TEST(MemoryRegionPolicy, LinuxHugePagePreferenceReportsSuccessOrFallback) {
    rt::Runtime runtime;
    ASSERT_EQ(runtime.register_callback({"phase", &count_callback, nullptr}),
              rt::Status::ok);
    rt::MemoryPolicyRequest request{};
    request.id = {rt::MemoryCategory::phase_scratch,
                  rt::ThreadRole::none, 0};
    request.policy.requirement = rt::PolicyRequirement::required;
    request.policy.huge_pages = rt::HugePagePolicy::prefer;
    request.policy.allow_huge_page_fallback = true;
    request.policy.rollback = rt::RollbackIntent::release;
    const std::array requests{request};
    ASSERT_EQ(runtime.set_cpu_memory_policy({{}, requests}), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    const auto report = find_memory_report(
        runtime, rt::MemoryCategory::phase_scratch);
    EXPECT_TRUE(report.huge_page_fallback ||
                report.verified.huge_pages == rt::HugePagePolicy::prefer);
}

TEST(MemoryRegionPolicy, RequiredStackFailureRollsBackBeforeCallbackAndRetries) {
    InjectedMemoryProvider provider;
    provider.fail_category = rt::MemoryCategory::thread_stack;
    int callbacks = 0;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_memory_region_provider(provider), rt::Status::ok);
    ASSERT_EQ(runtime.register_callback({"phase", &count_callback, &callbacks}),
              rt::Status::ok);
    rt::ThreadPolicyRequest stack_request{};
    stack_request.id = {rt::ThreadRole::executor_worker, 0};
    stack_request.policy.requirement = rt::PolicyRequirement::required;
    stack_request.policy.stack_bytes =
        std::max<std::size_t>(64 * 1024, PTHREAD_STACK_MIN * 2);
    stack_request.policy.guard_bytes = 4096;
    const std::array thread_requests{stack_request};
    ASSERT_EQ(runtime.set_cpu_memory_policy({thread_requests, {}}),
              rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(runtime.start(), rt::Status::resource_exhausted);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_EQ(callbacks, 0);
    auto report = find_memory_report(
        runtime,
        rt::MemoryCategory::thread_stack,
        rt::ThreadRole::executor_worker,
        0);
    EXPECT_EQ(report.application_status, rt::Status::resource_exhausted);
    EXPECT_EQ(report.application_system_error, 73);
    EXPECT_TRUE(report.rolled_back);
    EXPECT_EQ(report.committed_bytes, 0u);

    ASSERT_EQ(runtime.start(), rt::Status::ok) << runtime.last_error();
    ASSERT_EQ(runtime.step({0, std::chrono::nanoseconds{1}, {}}),
              rt::Status::ok);
    EXPECT_EQ(callbacks, 1);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MemoryRegionPolicy, OwnedWorkerStacksReleaseInReverseCreationOrder) {
    InjectedMemoryProvider provider;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.set_memory_region_provider(provider), rt::Status::ok);
    rt::RuntimeConfig config{};
    config.worker_count = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.register_callback({"phase", &count_callback, nullptr}),
              rt::Status::ok);
    std::array<rt::ThreadPolicyRequest, 2> stack_requests{};
    for (std::size_t index = 0; index < stack_requests.size(); ++index) {
        stack_requests[index].id = {
            rt::ThreadRole::executor_worker,
            static_cast<std::uint32_t>(index)};
        stack_requests[index].policy.requirement =
            rt::PolicyRequirement::required;
        stack_requests[index].policy.stack_bytes =
            std::max<std::size_t>(64 * 1024, PTHREAD_STACK_MIN * 2);
    }
    ASSERT_EQ(runtime.set_cpu_memory_policy({stack_requests, {}}),
              rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    ASSERT_GE(provider.releases.size(), 2u);
    EXPECT_EQ(provider.releases[0].category,
              rt::MemoryCategory::thread_stack);
    EXPECT_EQ(provider.releases[0].instance, 1u);
    EXPECT_EQ(provider.releases[1].category,
              rt::MemoryCategory::thread_stack);
    EXPECT_EQ(provider.releases[1].instance, 0u);
}

TEST(MemoryRegionPolicy, LinuxOwnedWorkerUsesAndReleasesCustomStack) {
    rt::Runtime runtime;
    int callbacks = 0;
    ASSERT_EQ(runtime.register_callback({"phase", &count_callback, &callbacks}),
              rt::Status::ok);
    rt::ThreadPolicyRequest stack_request{};
    stack_request.id = {rt::ThreadRole::executor_worker, 0};
    stack_request.policy.requirement = rt::PolicyRequirement::required;
    stack_request.policy.stack_bytes =
        std::max<std::size_t>(64 * 1024, PTHREAD_STACK_MIN * 2);
    stack_request.policy.guard_bytes = 4096;
    const std::array thread_requests{stack_request};
    ASSERT_EQ(runtime.set_cpu_memory_policy({thread_requests, {}}),
              rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok) << runtime.last_error();
    auto report = find_memory_report(
        runtime,
        rt::MemoryCategory::thread_stack,
        rt::ThreadRole::executor_worker,
        0);
    EXPECT_EQ(report.application, rt::PolicyStageState::applied);
    EXPECT_EQ(report.verification, rt::PolicyStageState::verified);
    EXPECT_GT(report.committed_bytes, stack_request.policy.stack_bytes);
    EXPECT_FALSE(report.rolled_back);
    ASSERT_EQ(runtime.step({0, std::chrono::nanoseconds{1}, {}}),
              rt::Status::ok);
    EXPECT_EQ(callbacks, 1);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    report = find_memory_report(
        runtime,
        rt::MemoryCategory::thread_stack,
        rt::ThreadRole::executor_worker,
        0);
    EXPECT_EQ(report.committed_bytes, 0u);
    EXPECT_FALSE(report.rolled_back);
}
#endif

} // namespace

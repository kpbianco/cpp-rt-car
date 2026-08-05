#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#endif

#include <rt/runtime.hpp>

namespace rt::detail {

struct ThreadRolePlan {
    ThreadRoleId role{};
    ThreadPolicy requested{};
    ThreadPolicy resolved{};
    PolicyResolutionState resolution = PolicyResolutionState::portable_default;
    MemoryPolicy stack_requested{};
    MemoryPolicy stack_resolved{};
    PolicyResolutionState stack_resolution =
        PolicyResolutionState::portable_default;
};

struct ThreadStartupResult {
    std::atomic<bool> ready{false};
    PolicyOperationState applied = PolicyOperationState::not_attempted;
    PolicyOperationState verified = PolicyOperationState::not_attempted;
    ThreadPolicy read_back{};
    std::int32_t creation_error = 0;
    std::int32_t apply_error = 0;
    std::int32_t verify_error = 0;
    bool used_default_fallback = false;
    PolicyOperationState stack_applied = PolicyOperationState::not_attempted;
    PolicyOperationState stack_verified = PolicyOperationState::not_attempted;
    PolicyOperationState stack_cleanup = PolicyOperationState::not_attempted;
    std::byte* stack_mapping_base = nullptr;
    std::size_t stack_mapping_bytes = 0;
    std::size_t stack_usable_bytes = 0;
    std::size_t stack_guard_bytes = 0;
    std::size_t stack_resident_bytes = 0;
    std::size_t stack_locked_bytes = 0;
    std::int32_t stack_apply_error = 0;
    std::int32_t stack_verify_error = 0;
    std::int32_t stack_cleanup_error = 0;
    bool stack_lock_applied = false;

    void reset() noexcept;
    void publish() noexcept;
    void wait() const noexcept;
};

class ThreadStartupGate final {
public:
    void reset() noexcept;
    void commit() noexcept;
    void abort() noexcept;
    [[nodiscard]] bool wait() const noexcept;

private:
    // 0 is pending, 1 commits, and 2 aborts.
    std::atomic<std::uint8_t> decision_{0};
};

class ThreadPolicyProvider {
public:
    virtual ~ThreadPolicyProvider() = default;

    [[nodiscard]] virtual Status resolve(
        ThreadRoleId role,
        PolicyApplicationMode mode,
        bool active,
        bool observable,
        bool has_request,
        const ThreadPolicy& requested,
        const ThreadPolicy& role_default,
        ThreadPolicy& resolved,
        PolicyResolutionState& resolution,
        std::int32_t& system_error) noexcept = 0;

    [[nodiscard]] virtual Status before_create(
        ThreadRoleId role,
        std::size_t instance_index,
        const ThreadRolePlan& plan,
        std::int32_t& system_error) noexcept;

    virtual void apply_and_verify_current(
        ThreadRoleId role,
        std::size_t instance_index,
        const ThreadRolePlan& plan,
        ThreadStartupResult& result) noexcept = 0;

    virtual void verify_current(
        ThreadRoleId role,
        const ThreadRolePlan& plan,
        ThreadStartupResult& result) noexcept = 0;

    virtual void apply_and_verify_stack_current(
        ThreadRoleId role,
        std::size_t instance_index,
        const ThreadRolePlan& plan,
        ThreadStartupResult& result) noexcept;

    [[nodiscard]] virtual Status cleanup_stack_current(
        ThreadRoleId role,
        std::size_t instance_index,
        const ThreadRolePlan& plan,
        ThreadStartupResult& result) noexcept;

    virtual void after_join(
        ThreadRoleId role,
        std::size_t instance_index) noexcept;
};

class NativeThreadPolicyProvider final : public ThreadPolicyProvider {
public:
    [[nodiscard]] Status resolve(
        ThreadRoleId role,
        PolicyApplicationMode mode,
        bool active,
        bool observable,
        bool has_request,
        const ThreadPolicy& requested,
        const ThreadPolicy& role_default,
        ThreadPolicy& resolved,
        PolicyResolutionState& resolution,
        std::int32_t& system_error) noexcept override;

    void apply_and_verify_current(
        ThreadRoleId role,
        std::size_t instance_index,
        const ThreadRolePlan& plan,
        ThreadStartupResult& result) noexcept override;

    void verify_current(
        ThreadRoleId role,
        const ThreadRolePlan& plan,
        ThreadStartupResult& result) noexcept override;
};

using NativeThreadEntry = void (*)(void*) noexcept;

class NativeThread final {
public:
    NativeThread() = default;
    ~NativeThread();

    NativeThread(const NativeThread&) = delete;
    NativeThread& operator=(const NativeThread&) = delete;
    NativeThread(NativeThread&&) = delete;
    NativeThread& operator=(NativeThread&&) = delete;

    [[nodiscard]] Status start(
        ThreadPolicyProvider& provider,
        ThreadStartupGate& gate,
        const ThreadRolePlan& plan,
        std::size_t instance_index,
        ThreadStartupResult& result,
        NativeThreadEntry entry,
        void* entry_data) noexcept;
    [[nodiscard]] Status cleanup_and_join() noexcept;
    void join() noexcept;
    [[nodiscard]] bool joinable() const noexcept;
    void wait_started() const noexcept;
    void wait_quiescent() const noexcept;

private:
    static void run_entry(NativeThread& self) noexcept;
#if defined(__linux__)
    static void* pthread_entry(void* self) noexcept;
    pthread_t thread_{};
    bool joinable_ = false;
#else
    std::thread thread_{};
#endif
    ThreadPolicyProvider* provider_ = nullptr;
    ThreadStartupGate* gate_ = nullptr;
    ThreadRolePlan plan_{};
    ThreadStartupResult* result_ = nullptr;
    std::size_t instance_index_ = 0;
    ThreadRoleId role_{};
    NativeThreadEntry entry_ = nullptr;
    void* entry_data_ = nullptr;
    std::atomic<bool> body_started_{false};
    std::atomic<bool> body_quiescent_{false};
    std::atomic<std::uint64_t> cleanup_request_{0};
    std::atomic<std::uint64_t> cleanup_complete_{0};
    std::atomic<std::int32_t> cleanup_status_{
        static_cast<std::int32_t>(Status::ok)};
    bool cleanup_succeeded_ = false;
};

[[nodiscard]] ThreadRolePlan make_thread_role_plan(
    const ThreadPolicyReport& report) noexcept;

[[nodiscard]] ThreadRolePlan make_thread_role_plan(
    const ThreadPolicyReport& report,
    const MemoryPolicyReport& stack_report) noexcept;

void reset_thread_report_operations(ThreadPolicyReport& report) noexcept;

void aggregate_thread_startup_results(
    ThreadPolicyReport& report,
    const ThreadStartupResult* results,
    std::size_t count) noexcept;

[[nodiscard]] Status aggregate_stack_startup_results(
    MemoryPolicyReport& report,
    const ThreadStartupResult* results,
    std::size_t count) noexcept;

[[nodiscard]] Status aggregate_runtime_stack_startup_results(
    MemoryPolicyReport& report,
    const ThreadStartupResult* executor_results,
    std::size_t executor_count,
    const ThreadStartupResult* watchdog_results,
    std::size_t watchdog_count,
    const ThreadStartupResult* device_results,
    std::size_t device_count) noexcept;

struct RuntimeThreadPolicyTestAccess {
    static void set_provider(
        Runtime& runtime,
        ThreadPolicyProvider& provider) noexcept;
};

} // namespace rt::detail

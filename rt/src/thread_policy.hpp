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
    void join() noexcept;
    [[nodiscard]] bool joinable() const noexcept;
    void wait_started() const noexcept;

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
    const ThreadRolePlan* plan_ = nullptr;
    ThreadStartupResult* result_ = nullptr;
    std::size_t instance_index_ = 0;
    ThreadRoleId role_{};
    NativeThreadEntry entry_ = nullptr;
    void* entry_data_ = nullptr;
    std::atomic<bool> body_started_{false};
};

[[nodiscard]] ThreadRolePlan make_thread_role_plan(
    const ThreadPolicyReport& report) noexcept;

void reset_thread_report_operations(ThreadPolicyReport& report) noexcept;

void aggregate_thread_startup_results(
    ThreadPolicyReport& report,
    const ThreadStartupResult* results,
    std::size_t count) noexcept;

struct RuntimeThreadPolicyTestAccess {
    static void set_provider(
        Runtime& runtime,
        ThreadPolicyProvider& provider) noexcept;
};

} // namespace rt::detail

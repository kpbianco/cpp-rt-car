#include "thread_policy_transaction.hpp"

#include <algorithm>

namespace {

bool has_native_fields(const rt::ThreadPolicy& policy) noexcept {
    return policy.cpu_set.specified ||
           policy.scheduling_class != rt::SchedulingClass::inherit ||
           policy.name.front() != '\0';
}

bool has_applied_fields(const rt::ThreadPolicy& policy) noexcept {
    return has_native_fields(policy) ||
           policy.wait_strategy != rt::WaitStrategy::runtime_default;
}

bool observed_matches(
    const rt::ThreadPolicy& resolved,
    const rt::ThreadPolicy& observed) noexcept {
    if (resolved.cpu_set.specified &&
        (!observed.cpu_set.specified ||
         resolved.cpu_set.words != observed.cpu_set.words)) {
        return false;
    }
    if (resolved.scheduling_class != rt::SchedulingClass::inherit &&
        (resolved.scheduling_class != observed.scheduling_class ||
         resolved.scheduling_priority != observed.scheduling_priority)) {
        return false;
    }
    if (resolved.name.front() != '\0' &&
        resolved.name != observed.name) {
        return false;
    }
    return true;
}

void copy_native_fields(
    const rt::ThreadPolicy& source,
    rt::ThreadPolicy& destination) noexcept {
    destination.cpu_set = source.cpu_set;
    destination.scheduling_class = source.scheduling_class;
    destination.scheduling_priority = source.scheduling_priority;
    destination.name = source.name;
}

} // namespace

namespace rt::detail {

void ThreadPolicyTransaction::begin(
    ThreadPolicyProvider& provider,
    std::span<ThreadPolicyReport> reports) noexcept {
    provider_ = &provider;
    reports_ = reports;
    for (auto& report : reports_) {
        report.applied = {};
        report.verified = {};
        report.application = PolicyStageState::not_performed;
        report.verification = report.ownership == ThreadOwnership::runtime
            ? PolicyStageState::not_performed
            : PolicyStageState::verify_only;
        report.application_status = Status::ok;
        report.verification_status = Status::ok;
        report.application_system_error = 0;
        report.verification_system_error = 0;
        report.rolled_back = false;
    }
    arrived_.store(0, std::memory_order_relaxed);
    released_.store(0, std::memory_order_relaxed);
    failure_.store(Status::ok, std::memory_order_relaxed);
    decision_.store(Decision::pending, std::memory_order_release);
}

Status ThreadPolicyTransaction::verify_frame_thread() noexcept {
    auto* report = find(ThreadResourceId{ThreadRole::frame, 0});
    if (!report || !provider_) {
        record_failure(Status::internal_error);
        return Status::internal_error;
    }
    if (!report->explicitly_requested ||
        !has_native_fields(report->resolved)) {
        return Status::ok;
    }
    ThreadPolicy observed{};
    int system_error = 0;
    const auto status = provider_->inspect_current_thread(
        report->id,
        observed,
        system_error);
    report->verified = observed;
    report->verification_status = status;
    report->verification_system_error = system_error;
    if (status == Status::ok &&
        observed_matches(report->resolved, observed)) {
        report->verification = PolicyStageState::verified;
        return Status::ok;
    }
    if (status == Status::ok) {
        report->verification_status = Status::invalid_state;
    }
    if (report->requested.requirement == PolicyRequirement::required) {
        record_failure(report->verification_status);
        return report->verification_status;
    }
    return Status::ok;
}

void ThreadPolicyTransaction::prepare_current_thread(
    ThreadResourceId id) noexcept {
    auto* report = find(id);
    if (!report || !provider_ ||
        report->ownership != ThreadOwnership::runtime) {
        record_failure(Status::internal_error);
        arrived_.fetch_add(1, std::memory_order_release);
        arrived_.notify_all();
        return;
    }

    const bool native_fields = has_native_fields(report->resolved);
    const bool wait_field = report->resolved.wait_strategy !=
        WaitStrategy::runtime_default;
    ThreadPolicy applied{};
    applied.requirement = report->resolved.requirement;
    applied.wait_strategy = report->resolved.wait_strategy;
    int application_error = 0;
    Status application_status = Status::ok;
    if (native_fields) {
        ThreadPolicy native_applied{};
        application_status = provider_->apply_current_thread(
            id,
            report->resolved,
            native_applied,
            application_error);
        copy_native_fields(native_applied, applied);
    }
    report->applied = applied;
    report->application_status = application_status;
    report->application_system_error = application_error;
    if ((native_fields || wait_field) &&
        application_status == Status::ok) {
        report->application = PolicyStageState::applied;
    }

    ThreadPolicy observed{};
    observed.requirement = report->resolved.requirement;
    observed.wait_strategy = report->resolved.wait_strategy;
    int verification_error = 0;
    Status verification_status = Status::ok;
    if (native_fields) {
        ThreadPolicy native_observed{};
        verification_status = provider_->inspect_current_thread(
            id,
            native_observed,
            verification_error);
        copy_native_fields(native_observed, observed);
    }
    report->verified = observed;
    report->verification_status = verification_status;
    report->verification_system_error = verification_error;
    if (verification_status == Status::ok &&
        observed_matches(report->resolved, observed) &&
        application_status == Status::ok) {
        if (native_fields || wait_field) {
            report->verification = PolicyStageState::verified;
        }
    } else if (verification_status == Status::ok) {
        report->verification_status = Status::invalid_state;
    }

    if (report->requested.requirement == PolicyRequirement::required) {
        if (application_status != Status::ok) {
            record_failure(application_status);
        } else if (report->verification_status != Status::ok) {
            record_failure(report->verification_status);
        }
    }
    arrived_.fetch_add(1, std::memory_order_release);
    arrived_.notify_all();
}

bool ThreadPolicyTransaction::await_decision(ThreadResourceId id) noexcept {
    auto decision = decision_.load(std::memory_order_acquire);
    while (decision == Decision::pending) {
        decision_.wait(decision, std::memory_order_acquire);
        decision = decision_.load(std::memory_order_acquire);
    }
    if (decision == Decision::abort) {
        if (auto* report = find(id);
            report && has_applied_fields(report->applied)) {
            report->rolled_back = true;
        }
    }
    released_.fetch_add(1, std::memory_order_release);
    released_.notify_all();
    return decision == Decision::commit;
}

void ThreadPolicyTransaction::commit() noexcept {
    decision_.store(Decision::commit, std::memory_order_release);
    decision_.notify_all();
    wait_for_released();
}

void ThreadPolicyTransaction::abort() noexcept {
    decision_.store(Decision::abort, std::memory_order_release);
    decision_.notify_all();
    wait_for_released();
}

Status ThreadPolicyTransaction::failure() const noexcept {
    return failure_.load(std::memory_order_acquire);
}

WaitStrategy ThreadPolicyTransaction::wait_strategy(
    ThreadResourceId id) const noexcept {
    const auto* report = find(id);
    return report ? report->resolved.wait_strategy
                  : WaitStrategy::runtime_default;
}

ThreadPolicyReport* ThreadPolicyTransaction::find(
    ThreadResourceId id) noexcept {
    const auto found = std::find_if(
        reports_.begin(),
        reports_.end(),
        [&](const auto& report) { return report.id == id; });
    return found == reports_.end() ? nullptr : &*found;
}

const ThreadPolicyReport* ThreadPolicyTransaction::find(
    ThreadResourceId id) const noexcept {
    const auto found = std::find_if(
        reports_.begin(),
        reports_.end(),
        [&](const auto& report) { return report.id == id; });
    return found == reports_.end() ? nullptr : &*found;
}

void ThreadPolicyTransaction::record_failure(Status status) noexcept {
    auto expected = Status::ok;
    (void)failure_.compare_exchange_strong(
        expected,
        status,
        std::memory_order_acq_rel,
        std::memory_order_relaxed);
}

void ThreadPolicyTransaction::wait_for_released() noexcept {
    const auto target = arrived_.load(std::memory_order_acquire);
    auto current = released_.load(std::memory_order_acquire);
    while (current < target) {
        released_.wait(current, std::memory_order_acquire);
        current = released_.load(std::memory_order_acquire);
    }
}

} // namespace rt::detail

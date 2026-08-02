#include "owned_thread.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

namespace rt::detail {

OwnedThread::~OwnedThread() {
    join();
}

OwnedThread::OwnedThread(OwnedThread&& other) noexcept {
    move_from(other);
}

OwnedThread& OwnedThread::operator=(OwnedThread&& other) noexcept {
    if (this != &other) {
        join();
        move_from(other);
    }
    return *this;
}

Status OwnedThread::start(
    MemoryRegionProvider& provider,
    std::span<MemoryRegionPolicyReport> reports,
    ThreadResourceId id,
    Entry entry,
    void* context,
    std::size_t index) noexcept {
    if (joinable() || entry == nullptr) {
        return Status::invalid_state;
    }
    const MemoryRegionId stack_id{
        MemoryCategory::thread_stack,
        id.role,
        id.instance,
    };
    const auto found = std::find_if(
        reports.begin(),
        reports.end(),
        [&](const auto& report) { return report.id == stack_id; });
    if (found == reports.end()) {
        return Status::internal_error;
    }

    entry_ = entry;
    context_ = context;
    index_ = index;
    stack_report_ = &*found;
    if (found->reported_bytes != 0) {
        const auto status = stack_storage_.create(
            provider,
            stack_id,
            found->reported_bytes,
            std::max<std::size_t>(
                alignof(std::max_align_t),
                found->resolved.alignment),
            *found);
        if (status != Status::ok) {
            entry_ = nullptr;
            context_ = nullptr;
            stack_report_ = nullptr;
            return status;
        }
    }

#if defined(__linux__)
    if (stack_storage_.owns_allocation()) {
        pthread_attr_t attributes{};
        auto error = ::pthread_attr_init(&attributes);
        const bool attributes_initialized = error == 0;
        if (error == 0) {
            error = ::pthread_attr_setstack(
                &attributes,
                stack_storage_.data(),
                stack_storage_.size());
        }
        if (error == 0) {
            error = ::pthread_create(
                &pthread_,
                &attributes,
                &OwnedThread::pthread_entry,
                this);
        }
        if (attributes_initialized) {
            (void)::pthread_attr_destroy(&attributes);
        }
        if (error != 0) {
            if (stack_report_ != nullptr) {
                stack_report_->application_status =
                    Status::resource_exhausted;
                stack_report_->application_system_error = error;
                (void)stack_storage_.reset(stack_report_);
            }
            entry_ = nullptr;
            context_ = nullptr;
            stack_report_ = nullptr;
            return Status::resource_exhausted;
        }
        pthread_joinable_ = true;
        return Status::ok;
    }
#endif

    try {
        portable_thread_ = std::thread([this] { run(); });
    } catch (const std::bad_alloc&) {
        entry_ = nullptr;
        context_ = nullptr;
        stack_report_ = nullptr;
        return Status::resource_exhausted;
    } catch (...) {
        entry_ = nullptr;
        context_ = nullptr;
        stack_report_ = nullptr;
        return Status::internal_error;
    }
    return Status::ok;
}

Status OwnedThread::start(
    Entry entry,
    void* context,
    std::size_t index) noexcept {
    if (joinable() || entry == nullptr) {
        return Status::invalid_state;
    }
    entry_ = entry;
    context_ = context;
    index_ = index;
    try {
        portable_thread_ = std::thread([this] { run(); });
    } catch (const std::bad_alloc&) {
        entry_ = nullptr;
        context_ = nullptr;
        return Status::resource_exhausted;
    } catch (...) {
        entry_ = nullptr;
        context_ = nullptr;
        return Status::internal_error;
    }
    return Status::ok;
}

void OwnedThread::join() noexcept {
#if defined(__linux__)
    if (pthread_joinable_) {
        (void)::pthread_join(pthread_, nullptr);
        pthread_joinable_ = false;
    }
#endif
    if (portable_thread_.joinable()) {
        portable_thread_.join();
    }
    if (stack_storage_.owns_allocation()) {
        (void)stack_storage_.reset(stack_report_);
    }
    stack_report_ = nullptr;
    entry_ = nullptr;
    context_ = nullptr;
}

bool OwnedThread::joinable() const noexcept {
#if defined(__linux__)
    if (pthread_joinable_) {
        return true;
    }
#endif
    return portable_thread_.joinable();
}

#if defined(__linux__)
void* OwnedThread::pthread_entry(void* opaque) noexcept {
    static_cast<OwnedThread*>(opaque)->run();
    return nullptr;
}
#endif

void OwnedThread::run() noexcept {
    entry_(context_, index_);
}

void OwnedThread::move_from(OwnedThread& other) noexcept {
    // Components reserve their OwnedThread vectors before starting. Moving a
    // live pthread would invalidate the trampoline's object address.
    if (other.joinable()) {
        std::terminate();
    }
    entry_ = other.entry_;
    context_ = other.context_;
    index_ = other.index_;
    stack_storage_ = std::move(other.stack_storage_);
    stack_report_ = other.stack_report_;
    portable_thread_ = std::move(other.portable_thread_);
    other.entry_ = nullptr;
    other.context_ = nullptr;
    other.stack_report_ = nullptr;
}

} // namespace rt::detail

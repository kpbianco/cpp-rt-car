#include "device_manager.hpp"

#include "aligned_storage.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include <rt/arch.hpp>

namespace {

constexpr std::uint32_t kOutstandingFree = 0;
constexpr std::uint32_t kOutstandingOwned = 1;
constexpr std::uint32_t kOutstandingSubmitting = 2;
constexpr std::uint32_t kOutstandingSubmitted = 3;
constexpr std::uint32_t kOutstandingEarlyReady = 4;

constexpr std::uint8_t kBackendOwnershipNone = 0;
constexpr std::uint8_t kBackendOwnershipInitialized = 1;
constexpr std::uint8_t kBackendOwnershipInitializationUncertain = 2;

bool bytes_zero(const std::uint64_t* values, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (values[index] != 0) {
            return false;
        }
    }
    return true;
}

bool valid_access(std::uint32_t access) noexcept {
    return access == RTFW_DEVICE_ACCESS_READ ||
           access == RTFW_DEVICE_ACCESS_WRITE ||
           access == RTFW_DEVICE_ACCESS_READ_WRITE;
}

} // namespace

namespace rt::detail {

Status device_status_to_runtime(rtfw_device_status status) noexcept {
    switch (status) {
    case RTFW_DEVICE_STATUS_OK:
        return Status::ok;
    case RTFW_DEVICE_STATUS_INVALID_ARGUMENT:
        return Status::invalid_argument;
    case RTFW_DEVICE_STATUS_INVALID_STATE:
        return Status::invalid_state;
    case RTFW_DEVICE_STATUS_QUEUE_FULL:
        return Status::device_queue_full;
    case RTFW_DEVICE_STATUS_TIMEOUT:
        return Status::device_timeout;
    case RTFW_DEVICE_STATUS_ERROR:
    case RTFW_DEVICE_STATUS_UNSUPPORTED:
    case RTFW_DEVICE_STATUS_INTERNAL_ERROR:
        return Status::device_error;
    case RTFW_DEVICE_STATUS_LOST:
        return Status::device_lost;
    case RTFW_DEVICE_STATUS_CANCELED:
        return Status::device_canceled;
    case RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED:
        return Status::resource_exhausted;
    case RTFW_DEVICE_STATUS_RESET_REQUIRED:
        return Status::device_reset_required;
    }
    return Status::device_error;
}

DeviceManager::DeviceManager(
    std::uint32_t owner,
    std::vector<DeviceBackendSpec> backends,
    std::vector<DeviceBufferSpec> buffers,
    std::size_t outstanding_capacity,
    std::size_t completion_batch)
    : owner_(owner),
      backends_(std::move(backends)),
      initialized_backends_(backends_.size(), 0),
      buffers_(std::move(buffers)),
      native_buffer_tokens_(buffers_.size(), 0),
      outstanding_slots_(
          outstanding_capacity == 0
              ? nullptr
              : std::make_unique<Outstanding[]>(
                    outstanding_capacity)),
      outstanding_capacity_(outstanding_capacity),
      completion_buffer_(
          completion_batch == 0
              ? nullptr
              : std::make_unique<rtfw_device_completion[]>(
                    completion_batch)),
      completion_batch_(completion_batch) {}

DeviceManager::~DeviceManager() {
    (void)stop();
}

bool DeviceManager::estimate_control_storage(
    std::size_t backend_count,
    std::size_t buffer_count,
    std::size_t outstanding_capacity,
    std::size_t completion_batch,
    std::size_t& bytes) noexcept {
    bytes = sizeof(DeviceManager);
    const auto add =
        [&bytes](std::size_t count, std::size_t size) {
            std::size_t product = 0;
            std::size_t total = 0;
            if (!checked_multiply(count, size, product) ||
                !checked_add(bytes, product, total)) {
                return false;
            }
            bytes = total;
            return true;
        };
    return add(backend_count, sizeof(DeviceBackendSpec)) &&
           add(backend_count, sizeof(std::uint8_t)) &&
           add(buffer_count, sizeof(DeviceBufferSpec)) &&
           add(buffer_count, sizeof(std::uint64_t)) &&
           add(outstanding_capacity, sizeof(Outstanding)) &&
           add(completion_batch, sizeof(rtfw_device_completion)) &&
           add(1, sizeof(std::thread));
}

Status DeviceManager::initialize_backends() noexcept {
    for (std::size_t index = 0;
         index < backends_.size();
         ++index) {
        auto& backend = backends_[index];
        rtfw_device_init_config config{};
        config.struct_size = sizeof(config);
        config.abi_version = RTFW_DEVICE_ABI_VERSION;
        config.requested_in_flight = outstanding_capacity_;
        std::size_t buffer_count = 0;
        for (const auto& buffer : buffers_) {
            buffer_count += buffer.backend_index == index ? 1u : 0u;
        }
        config.requested_registered_buffers = buffer_count;

        rtfw_device_status device_status =
            RTFW_DEVICE_STATUS_INTERNAL_ERROR;
        try {
            device_status = backend.api.initialize(
                backend.api.instance,
                &config);
        } catch (...) {
            device_status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
        }
        const auto status = device_status_to_runtime(device_status);
        if (status != Status::ok) {
            initialized_backends_[index] =
                kBackendOwnershipInitializationUncertain;
            (void)shutdown_backends();
            return status;
        }
        initialized_backends_[index] =
            kBackendOwnershipInitialized;
    }

    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        const auto& buffer = buffers_[index];
        auto& backend = backends_[buffer.backend_index];
        rtfw_device_buffer_registration registration{};
        registration.struct_size = sizeof(registration);
        registration.flags = buffer.flags;
        registration.data = buffer.storage.data();
        registration.bytes = buffer.storage.size();
        std::copy(
            buffer.name.begin(),
            buffer.name.end(),
            std::begin(registration.name));

        rtfw_device_status device_status =
            RTFW_DEVICE_STATUS_INTERNAL_ERROR;
        try {
            device_status = backend.api.register_buffer(
                backend.api.instance,
                &registration,
                &native_buffer_tokens_[index]);
        } catch (...) {
            device_status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
        }
        const auto status = device_status_to_runtime(device_status);
        if (status != Status::ok ||
            native_buffer_tokens_[index] == 0) {
            (void)shutdown_backends();
            return status == Status::ok
                ? Status::device_error
                : status;
        }
    }
    return Status::ok;
}

bool DeviceManager::backend_has_registered_buffers(
    std::size_t backend_index) const noexcept {
    for (std::size_t index = 0;
         index < buffers_.size();
         ++index) {
        if (buffers_[index].backend_index == backend_index &&
            native_buffer_tokens_[index] != 0) {
            return true;
        }
    }
    return false;
}

bool DeviceManager::has_backend_ownership() const noexcept {
    return std::any_of(
               native_buffer_tokens_.begin(),
               native_buffer_tokens_.end(),
               [](std::uint64_t token) {
                   return token != 0;
               }) ||
           std::any_of(
               initialized_backends_.begin(),
               initialized_backends_.end(),
               [](std::uint8_t initialized) {
                   return initialized != 0;
               });
}

bool DeviceManager::cleanup_pending() const noexcept {
    return has_backend_ownership();
}

Status DeviceManager::shutdown_backends() noexcept {
    Status first_failure = Status::ok;
    for (std::size_t index = buffers_.size(); index != 0; --index) {
        const auto buffer_index = index - 1;
        const auto token = native_buffer_tokens_[buffer_index];
        if (token == 0) {
            continue;
        }
        const auto backend_index =
            static_cast<std::size_t>(
                buffers_[buffer_index].backend_index);
        if (backend_index >= backends_.size() ||
            initialized_backends_[backend_index] ==
                kBackendOwnershipNone) {
            if (first_failure == Status::ok) {
                first_failure = Status::invalid_state;
            }
            continue;
        }
        auto& backend = backends_[backend_index];
        rtfw_device_status device_status =
            RTFW_DEVICE_STATUS_INTERNAL_ERROR;
        try {
            device_status = backend.api.unregister_buffer(
                backend.api.instance,
                token);
        } catch (...) {
            device_status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
        }
        const auto status = device_status_to_runtime(device_status);
        if (status == Status::ok) {
            native_buffer_tokens_[buffer_index] = 0;
        } else if (first_failure == Status::ok) {
            first_failure = status;
        }
    }
    for (std::size_t index = backends_.size(); index != 0; --index) {
        const auto backend_index = index - 1;
        const auto ownership =
            initialized_backends_[backend_index];
        if (ownership == kBackendOwnershipNone ||
            backend_has_registered_buffers(backend_index)) {
            continue;
        }
        auto& backend = backends_[backend_index];
        rtfw_device_status device_status =
            RTFW_DEVICE_STATUS_INTERNAL_ERROR;
        try {
            device_status =
                backend.api.shutdown(backend.api.instance);
        } catch (...) {
            device_status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
        }
        const auto status = device_status_to_runtime(device_status);
        if (status == Status::ok ||
            (ownership ==
                 kBackendOwnershipInitializationUncertain &&
             device_status == RTFW_DEVICE_STATUS_INVALID_STATE)) {
            initialized_backends_[backend_index] =
                kBackendOwnershipNone;
        } else if (first_failure == Status::ok) {
            first_failure = status;
        }
    }
    return first_failure;
}

Status DeviceManager::start(
    Executor& executor,
    DeviceEventObserver observer,
    void* observer_data) noexcept {
    if (started_.load(std::memory_order_acquire) ||
        has_backend_ownership() ||
        service_thread_.joinable() ||
        outstanding_capacity_ == 0 ||
        completion_batch_ == 0 ||
        backends_.empty()) {
        return Status::invalid_state;
    }
    const auto initialize_status = initialize_backends();
    if (initialize_status != Status::ok) {
        return initialize_status;
    }

    executor_ = &executor;
    observer_ = observer;
    observer_data_ = observer_data;
    stopping_.store(false, std::memory_order_release);
    service_ready_.store(false, std::memory_order_relaxed);
    started_.store(true, std::memory_order_release);
    try {
        service_thread_ = std::thread([this] {
            service_loop();
        });
    } catch (...) {
        started_.store(false, std::memory_order_release);
        executor_ = nullptr;
        observer_ = nullptr;
        observer_data_ = nullptr;
        (void)shutdown_backends();
        return Status::resource_exhausted;
    }
    while (!service_ready_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return Status::ok;
}

Status DeviceManager::stop() noexcept {
    const bool was_started =
        started_.exchange(false, std::memory_order_acq_rel);
    if (was_started || service_thread_.joinable()) {
        stopping_.store(true, std::memory_order_release);
        wake_sequence_.fetch_add(1, std::memory_order_release);
        wake_sequence_.notify_all();
        if (service_thread_.joinable()) {
            service_thread_.join();
        }
        for (std::size_t index = 0;
             index < outstanding_capacity_;
             ++index) {
            auto& slot = outstanding_slots_[index];
            const auto previous =
                slot.state.exchange(
                    kOutstandingFree,
                    std::memory_order_acq_rel);
            if (previous == kOutstandingSubmitted && executor_) {
                (void)executor_->complete_external(
                    slot.phase_index,
                    Status::device_canceled);
            }
        }
        outstanding_count_.store(0, std::memory_order_release);
        executor_ = nullptr;
        observer_ = nullptr;
        observer_data_ = nullptr;
    }
    return shutdown_backends();
}

DeviceManager::Outstanding* DeviceManager::acquire_outstanding() noexcept {
    const auto first =
        slot_hint_.fetch_add(1, std::memory_order_relaxed) %
        outstanding_capacity_;
    for (std::size_t offset = 0;
         offset < outstanding_capacity_;
         ++offset) {
        auto& slot = outstanding_slots_[
            (first + offset) % outstanding_capacity_];
        auto expected = kOutstandingFree;
        if (slot.state.compare_exchange_strong(
                expected,
                kOutstandingOwned,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return &slot;
        }
    }
    return nullptr;
}

Status DeviceManager::submit(
    std::size_t backend_index,
    std::size_t phase_index,
    std::size_t worker_index,
    std::uint64_t frame_index,
    const DeviceSubmission& requested,
    std::uint64_t& out_submission_id) noexcept {
    out_submission_id = 0;
    if (!started_.load(std::memory_order_acquire) ||
        backend_index >= backends_.size() ||
        requested.struct_size < sizeof(requested) ||
        requested.abi_version != RTFW_DEVICE_ABI_VERSION ||
        requested.submission_id != 0 ||
        requested.frame_index != 0 ||
        requested.timeout_ns == 0 ||
        requested.flags != 0 ||
        requested.payload_size >
            RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY ||
        requested.buffer_count >
            RTFW_DEVICE_BUFFER_REF_CAPACITY ||
        !bytes_zero(
            requested.reserved,
            std::size(requested.reserved))) {
        return Status::invalid_argument;
    }

    auto submission = requested;
    for (std::size_t index = 0;
         index < submission.buffer_count;
         ++index) {
        auto& reference = submission.buffers[index];
        const DeviceBufferHandle logical{reference.buffer_token};
        if (!logical.valid() ||
            logical.owner() != owner_ ||
            logical.index() >= buffers_.size() ||
            reference.reserved0 != 0 ||
            !valid_access(reference.access)) {
            return Status::invalid_handle;
        }
        const auto buffer_index =
            static_cast<std::size_t>(logical.index());
        const auto& buffer = buffers_[buffer_index];
        if (buffer.backend_index != backend_index ||
            reference.offset > buffer.storage.size() ||
            reference.bytes >
                buffer.storage.size() - reference.offset) {
            return Status::invalid_argument;
        }
        reference.buffer_token =
            native_buffer_tokens_[buffer_index];
    }

    auto* slot = acquire_outstanding();
    if (!slot) {
        queue_rejections_.fetch_add(1, std::memory_order_relaxed);
        return Status::device_queue_full;
    }
    auto submission_id =
        next_submission_id_.fetch_add(1, std::memory_order_relaxed);
    if (submission_id == 0) {
        submission_id =
            next_submission_id_.fetch_add(1, std::memory_order_relaxed);
    }
    slot->submission_id = submission_id;
    slot->frame_index = frame_index;
    slot->backend_index =
        static_cast<std::uint32_t>(backend_index);
    slot->phase_index =
        static_cast<std::uint32_t>(phase_index);
    slot->state.store(
        kOutstandingSubmitting,
        std::memory_order_release);

    submission.submission_id = submission_id;
    submission.frame_index = frame_index;
    rtfw_device_status device_status =
        RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    try {
        device_status = backends_[backend_index].api.submit(
            backends_[backend_index].api.instance,
            &submission);
    } catch (...) {
        device_status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    }
    const auto status = device_status_to_runtime(device_status);
    if (status != Status::ok) {
        const auto previous = slot->state.exchange(
            kOutstandingFree,
            std::memory_order_acq_rel);
        if (status == Status::device_queue_full) {
            queue_rejections_.fetch_add(1, std::memory_order_relaxed);
        } else {
            record_failure(status);
        }
        // Publishing a completion for a rejected submission violates the ABI
        // contract. Fail closed instead of applying an unaccepted result.
        return previous == kOutstandingEarlyReady
            ? Status::device_error
            : status;
    }

    outstanding_count_.fetch_add(1, std::memory_order_release);
    submissions_.fetch_add(1, std::memory_order_relaxed);
    out_submission_id = submission_id;
    auto submitted_event = DeviceEvent{
        DeviceEventKind::submitted,
        Status::ok,
        backend_index,
        phase_index,
        frame_index,
        submission_id,
        0,
    };
    submitted_event.producer = RuntimeTraceProducer::worker;
    submitted_event.worker_index = worker_index;
    emit(submitted_event);
    auto expected = kOutstandingSubmitting;
    if (!slot->state.compare_exchange_strong(
            expected,
            kOutstandingSubmitted,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        if (expected != kOutstandingEarlyReady) {
            fail_backend_outstanding(
                backend_index,
                Status::device_error);
            return Status::ok;
        }
        expected = kOutstandingEarlyReady;
        if (!slot->state.compare_exchange_strong(
                expected,
                kOutstandingOwned,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            fail_backend_outstanding(
                backend_index,
                Status::device_error);
            return Status::ok;
        }
        finalize_completion(
            *slot,
            backend_index,
            slot->early_completion);
        return Status::ok;
    }
    wake_sequence_.fetch_add(1, std::memory_order_release);
    wake_sequence_.notify_one();
    return Status::ok;
}

void DeviceManager::emit(const DeviceEvent& event) noexcept {
    if (observer_) {
        observer_(observer_data_, event);
    }
}

void DeviceManager::process_completion(
    std::size_t backend_index,
    const rtfw_device_completion& completion) noexcept {
    if (completion.struct_size < sizeof(completion) ||
        completion.submission_id == 0 ||
        !bytes_zero(
            completion.reserved,
            std::size(completion.reserved))) {
        fail_backend_outstanding(
            backend_index,
            Status::device_error);
        return;
    }
    for (std::size_t index = 0;
         index < outstanding_capacity_;
         ++index) {
        auto& slot = outstanding_slots_[index];
        auto state = slot.state.load(std::memory_order_acquire);
        if ((state != kOutstandingSubmitting &&
             state != kOutstandingSubmitted) ||
            slot.submission_id != completion.submission_id ||
            slot.backend_index != backend_index) {
            continue;
        }
        if (state == kOutstandingSubmitting) {
            slot.early_completion = completion;
            auto expected = kOutstandingSubmitting;
            if (slot.state.compare_exchange_strong(
                    expected,
                    kOutstandingEarlyReady,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                return;
            }
            state = expected;
        }
        if (state == kOutstandingSubmitted) {
            auto expected = kOutstandingSubmitted;
            if (slot.state.compare_exchange_strong(
                    expected,
                    kOutstandingOwned,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                finalize_completion(
                    slot,
                    backend_index,
                    completion);
                return;
            }
        }
        return;
    }
    failures_.fetch_add(1, std::memory_order_relaxed);
}

void DeviceManager::finalize_completion(
    Outstanding& slot,
    std::size_t backend_index,
    const rtfw_device_completion& completion) noexcept {
    const auto status =
        device_status_to_runtime(completion.status);
    completions_.fetch_add(1, std::memory_order_relaxed);
    if (status != Status::ok) {
        record_failure(status);
    }
    emit(DeviceEvent{
        DeviceEventKind::completed,
        status,
        backend_index,
        slot.phase_index,
        slot.frame_index,
        slot.submission_id,
        completion.device_timestamp_ns,
    });
    outstanding_count_.fetch_sub(1, std::memory_order_release);
    slot.state.store(kOutstandingFree, std::memory_order_release);
    if (executor_) {
        (void)executor_->complete_external(
            slot.phase_index,
            status);
    }
}

void DeviceManager::fail_backend_outstanding(
    std::size_t backend_index,
    Status status) noexcept {
    for (std::size_t index = 0;
         index < outstanding_capacity_;
         ++index) {
        auto& slot = outstanding_slots_[index];
        if (slot.state.load(std::memory_order_acquire) !=
                kOutstandingSubmitted ||
            slot.backend_index != backend_index) {
            continue;
        }
        auto expected = kOutstandingSubmitted;
        if (!slot.state.compare_exchange_strong(
                expected,
                kOutstandingOwned,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            continue;
        }
        record_failure(status);
        emit(DeviceEvent{
            DeviceEventKind::completed,
            status,
            backend_index,
            slot.phase_index,
            slot.frame_index,
            slot.submission_id,
            0,
        });
        if (executor_) {
            (void)executor_->complete_external(
                slot.phase_index,
                status);
        }
        outstanding_count_.fetch_sub(1, std::memory_order_release);
        slot.state.store(kOutstandingFree, std::memory_order_release);
    }
}

void DeviceManager::record_failure(Status status) noexcept {
    failures_.fetch_add(1, std::memory_order_relaxed);
    if (status == Status::device_timeout) {
        timeouts_.fetch_add(1, std::memory_order_relaxed);
    }
    if (status == Status::device_lost) {
        losses_.fetch_add(1, std::memory_order_relaxed);
    }
}

void DeviceManager::service_loop() noexcept {
    service_starts_.fetch_add(1, std::memory_order_release);
    service_ready_.store(true, std::memory_order_release);
    while (!stopping_.load(std::memory_order_acquire)) {
        if (outstanding_count_.load(std::memory_order_acquire) == 0) {
            const auto observed =
                wake_sequence_.load(std::memory_order_acquire);
            if (!stopping_.load(std::memory_order_acquire) &&
                outstanding_count_.load(std::memory_order_acquire) == 0) {
                wake_sequence_.wait(
                    observed,
                    std::memory_order_relaxed);
            }
            continue;
        }
        for (std::size_t backend_index = 0;
             backend_index < backends_.size();
             ++backend_index) {
            std::uint64_t count = 0;
            rtfw_device_status device_status =
                RTFW_DEVICE_STATUS_INTERNAL_ERROR;
            try {
                device_status = backends_[backend_index].api.poll(
                    backends_[backend_index].api.instance,
                    completion_buffer_.get(),
                    completion_batch_,
                    &count);
            } catch (...) {
                device_status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
            }
            service_polls_.fetch_add(1, std::memory_order_relaxed);
            const auto status =
                device_status_to_runtime(device_status);
            if (status != Status::ok ||
                count > completion_batch_) {
                fail_backend_outstanding(
                    backend_index,
                    status == Status::ok
                        ? Status::device_error
                        : status);
                continue;
            }
            for (std::size_t completion_index = 0;
                 completion_index <
                     static_cast<std::size_t>(count);
                 ++completion_index) {
                process_completion(
                    backend_index,
                    completion_buffer_[completion_index]);
            }
        }
        rt::cpu_relax();
    }
}

Status DeviceManager::health(
    std::size_t backend_index,
    DeviceHealth& output) noexcept {
    output = make_device_health();
    if (!started_.load(std::memory_order_acquire) ||
        backend_index >= backends_.size()) {
        return Status::invalid_state;
    }
    rtfw_device_status device_status =
        RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    try {
        device_status = backends_[backend_index].api.get_health(
            backends_[backend_index].api.instance,
            &output);
    } catch (...) {
        device_status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    }
    const auto status = device_status_to_runtime(device_status);
    if (status != Status::ok) {
        return status;
    }
    if (output.struct_size < sizeof(output) ||
        output.reserved0 != 0 ||
        output.state > RTFW_DEVICE_HEALTH_LOST ||
        !bytes_zero(output.reserved, std::size(output.reserved))) {
        output = make_device_health();
        return Status::device_error;
    }
    return Status::ok;
}

Status DeviceManager::reset(std::size_t backend_index) noexcept {
    if (!started_.load(std::memory_order_acquire) ||
        backend_index >= backends_.size()) {
        return Status::invalid_state;
    }
    for (std::size_t index = 0;
         index < outstanding_capacity_;
         ++index) {
        if (outstanding_slots_[index].state.load(
                std::memory_order_acquire) ==
                kOutstandingSubmitted &&
            outstanding_slots_[index].backend_index ==
                backend_index) {
            return Status::invalid_state;
        }
    }
    rtfw_device_status device_status =
        RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    try {
        device_status = backends_[backend_index].api.reset(
            backends_[backend_index].api.instance);
    } catch (...) {
        device_status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    }
    const auto status = device_status_to_runtime(device_status);
    if (status == Status::ok) {
        resets_.fetch_add(1, std::memory_order_relaxed);
        auto reset_event = DeviceEvent{
            DeviceEventKind::reset,
            status,
            backend_index,
            std::numeric_limits<std::size_t>::max(),
            0,
            0,
            0,
        };
        reset_event.producer = RuntimeTraceProducer::host;
        emit(reset_event);
    }
    return status;
}

DeviceManagerStats DeviceManager::stats() const noexcept {
    return DeviceManagerStats{
        submissions_.load(std::memory_order_acquire),
        completions_.load(std::memory_order_acquire),
        failures_.load(std::memory_order_acquire),
        queue_rejections_.load(std::memory_order_acquire),
        timeouts_.load(std::memory_order_acquire),
        losses_.load(std::memory_order_acquire),
        resets_.load(std::memory_order_acquire),
        service_polls_.load(std::memory_order_acquire),
        outstanding_count_.load(std::memory_order_acquire),
        service_starts_.load(std::memory_order_acquire),
    };
}

} // namespace rt::detail

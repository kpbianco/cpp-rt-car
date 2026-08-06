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

rt::HalV2Status runtime_status_to_hal(rt::Status status) noexcept {
    switch (status) {
    case rt::Status::device_queue_full:
        return rt::HalV2Status::queue_full;
    case rt::Status::device_timeout:
        return rt::HalV2Status::timeout;
    case rt::Status::device_lost:
        return rt::HalV2Status::lost;
    case rt::Status::device_canceled:
        return rt::HalV2Status::canceled;
    case rt::Status::device_reset_required:
        return rt::HalV2Status::reset_required;
    case rt::Status::resource_exhausted:
        return rt::HalV2Status::resource_exhausted;
    default:
        return rt::HalV2Status::error;
    }
}

} // namespace

namespace rt::detail {

DeviceManager::DeviceManager(std::uint32_t owner,
                             std::vector<DeviceBackendSpec> backends,
                             std::vector<DeviceBufferSpec> buffers,
                             std::size_t outstanding_capacity,
                             std::size_t completion_batch)
    : owner_(owner), backends_(std::move(backends)),
      initialized_backends_(backends_.size(), 0), buffers_(std::move(buffers)),
      native_memory_(buffers_.size()),
      outstanding_slots_(
          outstanding_capacity == 0
              ? nullptr
              : std::make_unique<Outstanding[]>(outstanding_capacity)),
      outstanding_capacity_(outstanding_capacity),
      completion_buffer_(
          completion_batch == 0
              ? nullptr
              : std::make_unique<HalV2Completion[]>(completion_batch)),
      completion_batch_(completion_batch) {}

DeviceManager::~DeviceManager() {
    (void)stop();
}

void DeviceManager::append_control_extents(
    std::vector<LogicalControlExtent>& extents,
    std::uint64_t& next_extent_id) const {
    const auto add = [&](const void* data, std::size_t count, std::size_t size) {
        if (count == 0) {
            return;
        }
        extents.push_back({
            next_extent_id++,
            ControlExtentOwner::device,
            data,
            count * size,
        });
    };
    add(this, 1, sizeof(*this));
    add(backends_.data(), backends_.capacity(), sizeof(backends_[0]));
    for (const auto& backend : backends_) {
        const auto begin = reinterpret_cast<std::uintptr_t>(&backend);
        const auto end = begin + sizeof(backend);
        const auto name = reinterpret_cast<std::uintptr_t>(
            backend.name.data());
        if (name < begin || name >= end) {
            add(backend.name.data(), backend.name.capacity() + 1, 1);
        }
        if (backend.v1_adapter) {
            add(backend.v1_adapter, 1, sizeof(*backend.v1_adapter));
            add(
                backend.v1_adapter->completion_storage(),
                backend.v1_adapter->completion_capacity(),
                sizeof(rtfw_device_completion));
        }
        if (backend.memory_state) {
          add(backend.memory_state, 1, sizeof(*backend.memory_state));
        }
    }
    add(initialized_backends_.data(), initialized_backends_.capacity(), sizeof(initialized_backends_[0]));
    add(buffers_.data(), buffers_.capacity(), sizeof(buffers_[0]));
    add(native_memory_.data(), native_memory_.capacity(),
        sizeof(native_memory_[0]));
    add(outstanding_slots_.get(), outstanding_capacity_, sizeof(Outstanding));
    add(completion_buffer_.get(), completion_batch_, sizeof(HalV2Completion));
}

bool DeviceManager::estimate_control_storage(
    std::size_t backend_count,
    std::size_t backend_name_bytes,
    std::size_t adapted_v1_count,
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
    std::size_t adapted_completion_count = 0;
    if (!checked_multiply(
            adapted_v1_count,
            completion_batch,
            adapted_completion_count)) {
        return false;
    }
    return add(backend_count, sizeof(DeviceBackendSpec)) &&
           add(backend_name_bytes, 1) &&
           add(adapted_v1_count, sizeof(DeviceV1CompatibilityAdapter)) &&
           add(adapted_completion_count, sizeof(rtfw_device_completion)) &&
           add(backend_count, sizeof(HeterogeneousMemoryState)) &&
           add(backend_count, sizeof(std::uint8_t)) &&
           add(buffer_count, sizeof(DeviceBufferSpec)) &&
           add(buffer_count, sizeof(NativeMemoryState)) &&
           add(outstanding_capacity, sizeof(Outstanding)) &&
           add(completion_batch, sizeof(HalV2Completion));
}

Status DeviceManager::initialize_backends() noexcept {
    for (std::size_t index = 0;
         index < backends_.size();
         ++index) {
        auto& backend = backends_[index];
        HalV2InitializeConfig config;
        config.requested_in_flight = outstanding_capacity_;
        std::size_t buffer_count = 0;
        for (const auto& buffer : buffers_) {
            buffer_count += buffer.backend_index == index ? 1u : 0u;
        }
        config.requested_registered_buffers = buffer_count;

        HalV2Status device_status = HalV2Status::internal_error;
        try {
            device_status = backend.api.initialize(
                backend.api.instance,
                &config);
        } catch (...) {
            device_status = HalV2Status::internal_error;
        }
        const auto status = hal_v2_status_to_runtime(device_status);
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
        auto &backend = backends_[buffer.backend_index];
        HalV2Status device_status = HalV2Status::internal_error;
        auto &native = native_memory_[index];
        if (buffer.heterogeneous) {
          if (!backend.memory_state ||
              !backend.memory_state->native_extension) {
            (void)shutdown_backends();
            return Status::invalid_state;
          }
          HalV2MemoryRegistration registration;
          registration.domain_identity = buffer.domain_identity;
          registration.bytes = buffer.bytes;
          registration.ownership = buffer.ownership;
          registration.access = buffer.flags;
          registration.coherency = buffer.coherency;
          registration.synchronization = buffer.synchronization;
          registration.host_data = buffer.storage.data();
          registration.opaque_handle = buffer.opaque_handle;
          std::copy(buffer.name.begin(), buffer.name.end(),
                    registration.name.begin());
          native.heterogeneous_owned = 1;
          try {
            device_status = backend.memory_state->extension.register_memory(
                backend.memory_state->extension.instance, &registration,
                &native.heterogeneous_token);
          } catch (...) {
            device_status = HalV2Status::internal_error;
          }
          if (device_status == HalV2Status::ok &&
              !validate_memory_token(native.heterogeneous_token, true)) {
            device_status = HalV2Status::internal_error;
          }
        } else {
          HalV2BufferRegistration registration;
          registration.flags = buffer.flags;
          registration.data = buffer.storage.data();
          registration.bytes = buffer.bytes;
          std::copy(buffer.name.begin(), buffer.name.end(),
                    registration.name.begin());
          try {
            device_status = backend.api.register_buffer(
                backend.api.instance, &registration, &native.legacy_token);
          } catch (...) {
            device_status = HalV2Status::internal_error;
          }
        }
        const auto status = hal_v2_status_to_runtime(device_status);
        if (status != Status::ok ||
            (!buffer.heterogeneous && native.legacy_token == 0)) {
          (void)shutdown_backends();
          return status == Status::ok ? Status::device_error : status;
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
          (native_memory_[index].legacy_token != 0 ||
           native_memory_[index].heterogeneous_owned != 0)) {
        return true;
      }
    }
    return false;
}

bool DeviceManager::has_backend_ownership() const noexcept {
  return std::any_of(native_memory_.begin(), native_memory_.end(),
                     [](const NativeMemoryState &state) {
                       return state.legacy_token != 0 ||
                              state.heterogeneous_owned != 0;
                     }) ||
         std::any_of(initialized_backends_.begin(), initialized_backends_.end(),
                     [](std::uint8_t initialized) { return initialized != 0; });
}

bool DeviceManager::cleanup_pending() const noexcept {
    return has_backend_ownership() || service_thread_.joinable();
}

Status DeviceManager::shutdown_backends() noexcept {
    Status first_failure = Status::ok;
    for (std::size_t index = buffers_.size(); index != 0; --index) {
        const auto buffer_index = index - 1;
        auto &native = native_memory_[buffer_index];
        if (native.legacy_token == 0 && native.heterogeneous_owned == 0) {
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
        HalV2Status device_status = HalV2Status::internal_error;
        if (buffers_[buffer_index].heterogeneous) {
          HalV2MemoryRegistration registration;
          const auto &buffer = buffers_[buffer_index];
          registration.domain_identity = buffer.domain_identity;
          registration.bytes = buffer.bytes;
          registration.ownership = buffer.ownership;
          registration.access = buffer.flags;
          registration.coherency = buffer.coherency;
          registration.synchronization = buffer.synchronization;
          registration.host_data = buffer.storage.data();
          registration.opaque_handle = buffer.opaque_handle;
          std::copy(buffer.name.begin(), buffer.name.end(),
                    registration.name.begin());
          try {
            device_status = backend.memory_state->extension.unregister_memory(
                backend.memory_state->extension.instance, &registration,
                &native.heterogeneous_token);
          } catch (...) {
            device_status = HalV2Status::internal_error;
          }
        } else {
          try {
            device_status = backend.api.unregister_buffer(backend.api.instance,
                                                          native.legacy_token);
          } catch (...) {
            device_status = HalV2Status::internal_error;
          }
        }
        const auto status = hal_v2_status_to_runtime(device_status);
        if (status == Status::ok) {
          native = {};
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
        HalV2Status device_status = HalV2Status::internal_error;
        try {
            device_status =
                backend.api.shutdown(backend.api.instance);
        } catch (...) {
            device_status = HalV2Status::internal_error;
        }
        const auto status = hal_v2_status_to_runtime(device_status);
        if (status == Status::ok ||
            (ownership ==
                 kBackendOwnershipInitializationUncertain &&
             device_status == HalV2Status::invalid_state)) {
            initialized_backends_[backend_index] =
                kBackendOwnershipNone;
        } else if (first_failure == Status::ok) {
            first_failure = status;
        }
    }
    return first_failure;
}

Status DeviceManager::start_lane(
    Executor& executor,
    DeviceEventObserver observer,
    void* observer_data,
    ThreadPolicyProvider& provider,
    ThreadStartupGate& gate,
    const ThreadRolePlan& plan) noexcept {
    if (started_.load(std::memory_order_acquire) ||
        has_backend_ownership() ||
        service_thread_.joinable() ||
        outstanding_capacity_ == 0 ||
        completion_batch_ == 0 ||
        backends_.empty()) {
        return Status::invalid_state;
    }
    executor_ = &executor;
    observer_ = observer;
    observer_data_ = observer_data;
    stopping_.store(false, std::memory_order_release);
    service_ready_.store(false, std::memory_order_relaxed);
    started_.store(true, std::memory_order_release);
    wait_strategy_ = plan.resolved.wait_strategy;
    const auto status = service_thread_.start(
        provider,
        gate,
        plan,
        0,
        startup_result_,
        &DeviceManager::service_entry,
        this);
    if (status != Status::ok) {
        started_.store(false, std::memory_order_release);
        executor_ = nullptr;
        observer_ = nullptr;
        observer_data_ = nullptr;
        return status;
    }
    return Status::ok;
}

Status DeviceManager::initialize() noexcept {
    if (!started_.load(std::memory_order_acquire)) {
        return Status::invalid_state;
    }
    return initialize_backends();
}

void DeviceManager::wait_started() const noexcept {
    service_thread_.wait_started();
}

Status DeviceManager::quiesce_lane() noexcept {
    const bool was_started =
        started_.exchange(false, std::memory_order_acq_rel);
    if (was_started || service_thread_.joinable()) {
        stopping_.store(true, std::memory_order_release);
        wake_sequence_.fetch_add(1, std::memory_order_release);
        wake_sequence_.notify_all();
        service_thread_.wait_quiescent();
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
    return Status::ok;
}

Status DeviceManager::cleanup_lane() noexcept {
    return service_thread_.cleanup_and_join();
}

Status DeviceManager::stop_lane() noexcept {
    const auto quiesce_status = quiesce_lane();
    const auto cleanup_status = cleanup_lane();
    return quiesce_status != Status::ok
        ? quiesce_status
        : cleanup_status;
}

Status DeviceManager::stop() noexcept {
    const auto quiesce_status = quiesce_lane();
    const auto backend_status = shutdown_backends();
    const auto cleanup_status = cleanup_lane();
    if (quiesce_status != Status::ok) {
        return quiesce_status;
    }
    if (backend_status != Status::ok) {
        return backend_status;
    }
    return cleanup_status;
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

    HalV2Submission submission;
    submission.timeout_ns = requested.timeout_ns;
    submission.opcode = requested.opcode;
    submission.flags = requested.flags;
    submission.payload_size = requested.payload_size;
    submission.buffer_count = requested.buffer_count;
    std::copy(
        std::begin(requested.payload),
        std::end(requested.payload),
        submission.payload.begin());
    for (std::size_t index = 0;
         index < requested.buffer_count;
         ++index) {
        const auto& requested_reference = requested.buffers[index];
        auto& reference = submission.buffers[index];
        const DeviceBufferHandle logical{requested_reference.buffer_token};
        if (!logical.valid() ||
            logical.owner() != owner_ ||
            logical.index() >= buffers_.size() ||
            requested_reference.reserved0 != 0 ||
            !valid_access(requested_reference.access)) {
            return Status::invalid_handle;
        }
        const auto buffer_index =
            static_cast<std::size_t>(logical.index());
        const auto& buffer = buffers_[buffer_index];
        const auto *domain =
            backends_[backend_index].memory_state
                ? find_memory_domain(*backends_[backend_index].memory_state,
                                     buffer.domain_identity)
                : nullptr;
        if (buffer.backend_index != backend_index || !domain ||
            requested_reference.offset > buffer.bytes ||
            requested_reference.bytes >
                buffer.bytes - requested_reference.offset ||
            requested_reference.offset % domain->offset_granularity != 0 ||
            requested_reference.bytes % domain->byte_granularity != 0) {
          return Status::invalid_argument;
        }
        const auto required_access =
            requested_reference.access == RTFW_DEVICE_ACCESS_READ
                ? RTFW_DEVICE_BUFFER_DEVICE_READ
            : requested_reference.access == RTFW_DEVICE_ACCESS_WRITE
                ? RTFW_DEVICE_BUFFER_DEVICE_WRITE
                : RTFW_DEVICE_BUFFER_DEVICE_READ |
                      RTFW_DEVICE_BUFFER_DEVICE_WRITE;
        if ((required_access & ~buffer.flags) != 0 ||
            (buffer.heterogeneous &&
             buffer.synchronization != hal_v2_memory_sync_none)) {
          return Status::device_error;
        }
        reference.buffer_token =
            buffer.heterogeneous ? native_memory_[buffer_index]
                                       .heterogeneous_token.submission_token
                                 : native_memory_[buffer_index].legacy_token;
        if (reference.buffer_token == 0) {
          return Status::invalid_state;
        }
        reference.access = requested_reference.access;
        reference.offset = requested_reference.offset;
        reference.bytes = requested_reference.bytes;
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
    HalV2Status device_status = HalV2Status::internal_error;
    try {
        device_status = backends_[backend_index].api.submit(
            backends_[backend_index].api.instance,
            &submission);
    } catch (...) {
        device_status = HalV2Status::internal_error;
    }
    const auto status = hal_v2_status_to_runtime(device_status);
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
    const HalV2Completion& completion) noexcept {
    if (!validate_hal_v2_completion(completion)) {
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
    const HalV2Completion& completion) noexcept {
    const auto status =
        hal_v2_status_to_runtime(
            static_cast<HalV2Status>(completion.status));
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
        auto state = slot.state.load(std::memory_order_acquire);
        if ((state != kOutstandingSubmitting &&
             state != kOutstandingSubmitted) ||
            slot.backend_index != backend_index) {
            continue;
        }
        if (state == kOutstandingSubmitting) {
            HalV2Completion completion;
            completion.status = static_cast<std::int32_t>(
                runtime_status_to_hal(status));
            completion.submission_id = slot.submission_id;
            slot.early_completion = completion;
            auto expected = kOutstandingSubmitting;
            if (slot.state.compare_exchange_strong(
                    expected,
                    kOutstandingEarlyReady,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                continue;
            }
            state = expected;
        }
        if (state != kOutstandingSubmitted) {
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
            if (wait_strategy_ == WaitStrategy::spin) {
                rt::cpu_relax();
                continue;
            }
            if (wait_strategy_ == WaitStrategy::yield) {
                std::this_thread::yield();
                continue;
            }
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
            std::fill_n(
                completion_buffer_.get(),
                completion_batch_,
                HalV2Completion{});
            for (std::size_t index = 0; index < completion_batch_; ++index) {
                completion_buffer_[index].struct_size = 0;
            }
            std::uint64_t count = 0;
            HalV2Status device_status = HalV2Status::internal_error;
            try {
                device_status = backends_[backend_index].api.poll(
                    backends_[backend_index].api.instance,
                    completion_buffer_.get(),
                    completion_batch_,
                    &count);
            } catch (...) {
                device_status = HalV2Status::internal_error;
            }
            service_polls_.fetch_add(1, std::memory_order_relaxed);
            const auto status =
                hal_v2_status_to_runtime(device_status);
            if (status != Status::ok ||
                count > completion_batch_) {
                fail_backend_outstanding(
                    backend_index,
                    status == Status::ok
                        ? Status::device_error
                        : status);
                continue;
            }
            bool batch_valid = true;
            for (std::size_t completion_index = 0;
                 completion_index < static_cast<std::size_t>(count);
                 ++completion_index) {
                const auto& completion =
                    completion_buffer_[completion_index];
                if (!validate_hal_v2_completion(completion)) {
                    batch_valid = false;
                    break;
                }
                for (std::size_t prior = 0;
                     prior < completion_index;
                     ++prior) {
                    if (completion_buffer_[prior].submission_id ==
                        completion.submission_id) {
                        batch_valid = false;
                        break;
                    }
                }
                bool matched = false;
                for (std::size_t slot_index = 0;
                     batch_valid && slot_index < outstanding_capacity_;
                     ++slot_index) {
                    const auto& slot = outstanding_slots_[slot_index];
                    const auto slot_state =
                        slot.state.load(std::memory_order_acquire);
                    if ((slot_state == kOutstandingSubmitting ||
                         slot_state == kOutstandingSubmitted) &&
                        slot.backend_index == backend_index &&
                        slot.submission_id == completion.submission_id) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    batch_valid = false;
                }
                if (!batch_valid) {
                    break;
                }
            }
            if (!batch_valid) {
                fail_backend_outstanding(
                    backend_index,
                    Status::device_error);
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

void DeviceManager::service_entry(void* manager) noexcept {
    static_cast<DeviceManager*>(manager)->service_loop();
}

Status DeviceManager::health(
    std::size_t backend_index,
    DeviceHealth& output) noexcept {
    output = make_device_health();
    if (!started_.load(std::memory_order_acquire) ||
        backend_index >= backends_.size()) {
        return Status::invalid_state;
    }
    HalV2Health hal_health;
    hal_health.reserved0 = std::numeric_limits<std::uint32_t>::max();
    HalV2Status device_status = HalV2Status::internal_error;
    try {
        device_status = backends_[backend_index].api.get_health(
            backends_[backend_index].api.instance,
            &hal_health);
    } catch (...) {
        device_status = HalV2Status::internal_error;
    }
    const auto status = hal_v2_status_to_runtime(device_status);
    if (status != Status::ok) {
        return status;
    }
    if (!validate_hal_v2_health(hal_health)) {
        return Status::device_error;
    }
    output.state = hal_health.state;
    output.last_status = hal_health.last_status;
    output.generation = hal_health.generation;
    output.submissions = hal_health.submissions;
    output.completions = hal_health.completions;
    output.queue_rejections = hal_health.queue_rejections;
    output.timeouts = hal_health.timeouts;
    output.errors = hal_health.errors;
    output.losses = hal_health.losses;
    output.cancellations = hal_health.cancellations;
    output.resets = hal_health.resets;
    output.outstanding = hal_health.outstanding;
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
    HalV2Status device_status = HalV2Status::internal_error;
    try {
        device_status = backends_[backend_index].api.reset(
            backends_[backend_index].api.instance);
    } catch (...) {
        device_status = HalV2Status::internal_error;
    }
    const auto status = hal_v2_status_to_runtime(device_status);
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

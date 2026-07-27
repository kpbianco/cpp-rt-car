#include <rt/xdma_backend.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr std::uint32_t kSlotFree = 0;
constexpr std::uint32_t kSlotOwned = 1;
constexpr std::uint32_t kSlotQueued = 2;
constexpr std::uint32_t kSlotRunning = 3;
constexpr std::uint32_t kSlotComplete = 4;
constexpr std::uint32_t kSlotReaping = 5;
constexpr std::size_t kAbsoluteCapacityLimit = std::size_t{1} << 20;
constexpr std::uint32_t kKnownBufferFlags =
    RTFW_DEVICE_BUFFER_HOST_READ |
    RTFW_DEVICE_BUFFER_HOST_WRITE |
    RTFW_DEVICE_BUFFER_DEVICE_READ |
    RTFW_DEVICE_BUFFER_DEVICE_WRITE;

bool words_zero(const std::uint64_t* values, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (values[index] != 0) {
            return false;
        }
    }
    return true;
}

bool valid_identifier(const char* value, std::size_t capacity) noexcept {
    if (!value || capacity == 0) {
        return false;
    }
    std::size_t length = 0;
    while (length < capacity && value[length] != '\0') {
        const char character = value[length];
        const bool valid =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '.' || character == '_' || character == ':' ||
            character == '/' || character == '@' || character == '-';
        if (!valid) {
            return false;
        }
        ++length;
    }
    if (length == 0 || length == capacity) {
        return false;
    }
    for (std::size_t index = length + 1; index < capacity; ++index) {
        if (value[index] != '\0') {
            return false;
        }
    }
    return true;
}

bool power_of_two(std::uint64_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

bool aligned(std::uint64_t value, std::uint64_t alignment) noexcept {
    return (value & (alignment - 1)) == 0;
}

bool driver_valid(const rt::XdmaDriverApi& driver) noexcept {
    return driver.struct_size >= sizeof(driver) &&
           driver.api_version == rt::xdma_driver_api_version &&
           driver.initialize &&
           driver.transfer &&
           driver.reset &&
           driver.shutdown &&
           driver.monotonic_time_ns &&
           words_zero(driver.reserved, std::size(driver.reserved));
}

rt::XdmaBackendConfig validated_config(
    const rt::XdmaDriverApi& driver,
    const rt::XdmaBackendConfig& config) {
    if (!driver_valid(driver) ||
        config.queue_capacity == 0 ||
        config.queue_capacity > kAbsoluteCapacityLimit ||
        config.buffer_capacity == 0 ||
        config.buffer_capacity > kAbsoluteCapacityLimit ||
        config.worker_count == 0 ||
        config.worker_count > config.queue_capacity ||
        (config.h2c_channel_count == 0 &&
         config.c2h_channel_count == 0) ||
        config.max_transfer_bytes == 0 ||
        config.max_buffer_bytes < config.max_transfer_bytes ||
        !power_of_two(config.transfer_alignment) ||
        config.transfer_alignment > config.max_transfer_bytes) {
        throw std::invalid_argument(
            "invalid XDMA backend driver or capacity configuration");
    }
    return config;
}

rtfw_device_status normalize(rt::XdmaDriverResult result) noexcept {
    switch (result) {
    case rt::XdmaDriverResult::success:
        return RTFW_DEVICE_STATUS_OK;
    case rt::XdmaDriverResult::invalid_value:
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    case rt::XdmaDriverResult::resource_exhausted:
        return RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED;
    case rt::XdmaDriverResult::timeout:
        return RTFW_DEVICE_STATUS_TIMEOUT;
    case rt::XdmaDriverResult::io_error:
        return RTFW_DEVICE_STATUS_ERROR;
    case rt::XdmaDriverResult::device_lost:
        return RTFW_DEVICE_STATUS_LOST;
    case rt::XdmaDriverResult::reset_required:
        return RTFW_DEVICE_STATUS_RESET_REQUIRED;
    case rt::XdmaDriverResult::error:
        return RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    }
    return RTFW_DEVICE_STATUS_INTERNAL_ERROR;
}

} // namespace

namespace rt {

struct XdmaDeviceBackend::Impl {
    struct Buffer {
        void* data = nullptr;
        std::uint64_t bytes = 0;
        std::uint32_t flags = 0;
        std::uint64_t token = 0;
        bool registered = false;
        std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
    };

    struct Slot {
        std::atomic<std::uint32_t> state{kSlotFree};
        std::atomic<bool> timed_out{false};
        std::uint64_t submission_id = 0;
        std::uint64_t started_ns = 0;
        std::uint64_t timeout_ns = 0;
        std::uint64_t device_offset = 0;
        std::uint64_t bytes = 0;
        std::uint64_t buffer_token = 0;
        std::uint64_t completed_ns = 0;
        std::uint64_t bytes_transferred = 0;
        std::uint32_t channel = 0;
        XdmaDirection direction = XdmaDirection::host_to_card;
        void* host_data = nullptr;
        XdmaDriverResult result = XdmaDriverResult::error;
    };

    Impl(
        const XdmaDriverApi& requested_driver,
        const XdmaBackendConfig& requested_config)
        : driver(requested_driver),
          config(validated_config(requested_driver, requested_config)),
          slots(std::make_unique<Slot[]>(config.queue_capacity)),
          buffers(std::make_unique<Buffer[]>(config.buffer_capacity)),
          workers(std::make_unique<std::thread[]>(config.worker_count)) {}

    ~Impl() {
        if (initialized.load(std::memory_order_acquire) ||
            shutdown_incomplete.load(std::memory_order_acquire)) {
            (void)shutdown(this);
        }
    }

    static Impl* self(void* instance) noexcept {
        return static_cast<Impl*>(instance);
    }

    Buffer* buffer_for(std::uint64_t token) noexcept {
        if (token == 0 || token > config.buffer_capacity) {
            return nullptr;
        }
        auto& buffer =
            buffers[static_cast<std::size_t>(token - 1)];
        return buffer.registered && buffer.token == token
            ? &buffer
            : nullptr;
    }

    const Buffer* buffer_for(std::uint64_t token) const noexcept {
        return const_cast<Impl*>(this)->buffer_for(token);
    }

    void escalate_health(rtfw_device_health_state requested) noexcept {
        auto current = health_state.load(std::memory_order_acquire);
        while (current < requested &&
               !health_state.compare_exchange_weak(
                   current,
                   requested,
                   std::memory_order_release,
                   std::memory_order_acquire)) {
        }
    }

    void account_completion(rtfw_device_status status) noexcept {
        last_status.store(status, std::memory_order_release);
        switch (status) {
        case RTFW_DEVICE_STATUS_OK:
            break;
        case RTFW_DEVICE_STATUS_TIMEOUT:
            timeouts.fetch_add(1, std::memory_order_relaxed);
            escalate_health(RTFW_DEVICE_HEALTH_DEGRADED);
            break;
        case RTFW_DEVICE_STATUS_LOST:
            losses.fetch_add(1, std::memory_order_relaxed);
            escalate_health(RTFW_DEVICE_HEALTH_LOST);
            break;
        case RTFW_DEVICE_STATUS_RESET_REQUIRED:
            errors.fetch_add(1, std::memory_order_relaxed);
            escalate_health(RTFW_DEVICE_HEALTH_RESET_REQUIRED);
            break;
        default:
            errors.fetch_add(1, std::memory_order_relaxed);
            escalate_health(RTFW_DEVICE_HEALTH_DEGRADED);
            break;
        }
    }

    bool active_slot_references(std::uint64_t token) const noexcept {
        for (std::size_t index = 0; index < active_queue_capacity; ++index) {
            const auto state =
                slots[index].state.load(std::memory_order_acquire);
            if (state != kSlotFree &&
                slots[index].buffer_token == token) {
                return true;
            }
        }
        return false;
    }

    bool unfinished_work_exists() const noexcept {
        for (std::size_t index = 0; index < active_queue_capacity; ++index) {
            const auto state =
                slots[index].state.load(std::memory_order_acquire);
            if (state == kSlotOwned ||
                state == kSlotQueued ||
                state == kSlotRunning) {
                return true;
            }
        }
        return false;
    }

    bool claim_and_execute(std::size_t& scan_hint) noexcept {
        for (std::size_t attempt = 0;
             attempt < active_queue_capacity;
             ++attempt) {
            const std::size_t index =
                (scan_hint + attempt) % active_queue_capacity;
            auto& slot = slots[index];
            auto expected = kSlotQueued;
            if (!slot.state.compare_exchange_strong(
                    expected,
                    kSlotRunning,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                continue;
            }
            scan_hint = (index + 1) % active_queue_capacity;
            XdmaTransferResult transfer{};
            if (slot.timed_out.load(std::memory_order_acquire)) {
                transfer.result = XdmaDriverResult::timeout;
            } else {
                transfer = driver.transfer(
                    driver.user_data,
                    slot.direction,
                    slot.channel,
                    slot.device_offset,
                    slot.host_data,
                    slot.bytes);
            }
            slot.result = transfer.result;
            slot.bytes_transferred = transfer.bytes_transferred;
            slot.completed_ns =
                driver.monotonic_time_ns(driver.user_data);
            slot.state.store(kSlotComplete, std::memory_order_release);
            completion_epoch.fetch_add(1, std::memory_order_release);
            completion_epoch.notify_all();
            return true;
        }
        return false;
    }

    void worker_loop(std::size_t worker_index) noexcept {
        std::size_t scan_hint =
            worker_index % active_queue_capacity;
        auto observed = work_epoch.load(std::memory_order_acquire);
        for (;;) {
            if (claim_and_execute(scan_hint)) {
                observed = work_epoch.load(std::memory_order_acquire);
                continue;
            }
            if (stop_requested.load(std::memory_order_acquire) &&
                !unfinished_work_exists()) {
                return;
            }
            const auto current =
                work_epoch.load(std::memory_order_acquire);
            if (current == observed) {
                work_epoch.wait(observed, std::memory_order_relaxed);
            }
            observed = work_epoch.load(std::memory_order_acquire);
        }
    }

    bool submission_valid(
        const rtfw_device_submission& submission,
        XdmaTransfer& transfer,
        Buffer*& buffer,
        XdmaDirection& direction) noexcept {
        if (submission.struct_size < sizeof(submission) ||
            submission.abi_version != RTFW_DEVICE_ABI_VERSION ||
            submission.submission_id == 0 ||
            submission.timeout_ns == 0 ||
            submission.flags != 0 ||
            submission.payload_size != sizeof(XdmaTransfer) ||
            submission.buffer_count != 1 ||
            !words_zero(
                submission.reserved,
                std::size(submission.reserved))) {
            return false;
        }
        std::memcpy(&transfer, submission.payload, sizeof(transfer));
        if (transfer.reserved0 != 0 ||
            !words_zero(transfer.reserved, std::size(transfer.reserved))) {
            return false;
        }

        if (submission.opcode == xdma_device_opcode_host_to_card) {
            direction = XdmaDirection::host_to_card;
            if (transfer.channel >= config.h2c_channel_count) {
                return false;
            }
        } else if (submission.opcode == xdma_device_opcode_card_to_host) {
            direction = XdmaDirection::card_to_host;
            if (transfer.channel >= config.c2h_channel_count) {
                return false;
            }
        } else {
            return false;
        }

        const auto& reference = submission.buffers[0];
        buffer = buffer_for(reference.buffer_token);
        if (!buffer ||
            reference.reserved0 != 0 ||
            reference.bytes == 0 ||
            reference.bytes > config.max_transfer_bytes ||
            reference.offset > buffer->bytes ||
            reference.bytes > buffer->bytes - reference.offset ||
            transfer.device_offset >
                std::numeric_limits<std::uint64_t>::max() -
                    reference.bytes ||
            !aligned(reference.offset, config.transfer_alignment) ||
            !aligned(reference.bytes, config.transfer_alignment) ||
            !aligned(
                transfer.device_offset,
                config.transfer_alignment)) {
            return false;
        }

        const auto host_address =
            reinterpret_cast<std::uintptr_t>(buffer->data);
        if (reference.offset >
                std::numeric_limits<std::uintptr_t>::max() -
                    host_address ||
            !aligned(
                static_cast<std::uint64_t>(
                    host_address +
                    static_cast<std::uintptr_t>(reference.offset)),
                config.transfer_alignment)) {
            return false;
        }

        if (direction == XdmaDirection::host_to_card) {
            return (reference.access & RTFW_DEVICE_ACCESS_READ) != 0 &&
                   (buffer->flags & RTFW_DEVICE_BUFFER_HOST_READ) != 0 &&
                   (buffer->flags & RTFW_DEVICE_BUFFER_DEVICE_WRITE) != 0;
        }
        return (reference.access & RTFW_DEVICE_ACCESS_WRITE) != 0 &&
               (buffer->flags & RTFW_DEVICE_BUFFER_HOST_WRITE) != 0 &&
               (buffer->flags & RTFW_DEVICE_BUFFER_DEVICE_READ) != 0;
    }

    static rtfw_device_status get_capabilities(
        void* instance,
        rtfw_device_capabilities* output) noexcept {
        auto* backend = self(instance);
        if (!backend || !output ||
            output->struct_size < sizeof(*output)) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        *output = {};
        output->struct_size = sizeof(*output);
        output->abi_version = RTFW_DEVICE_ABI_VERSION;
        output->max_in_flight = backend->config.queue_capacity;
        output->max_registered_buffers = backend->config.buffer_capacity;
        output->max_buffer_bytes = backend->config.max_buffer_bytes;
        output->inline_payload_capacity =
            RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY;
        output->buffer_ref_capacity =
            RTFW_DEVICE_BUFFER_REF_CAPACITY;
        output->supports_cancel = 0;
        output->supports_reset = 1;
        output->deterministic_mock = 0;
        constexpr std::string_view identifier =
            "rtfw.xdma.xilinx_linux_aximm.v1";
        std::copy(
            identifier.begin(),
            identifier.end(),
            output->backend_id);
        output->control_storage_bytes =
            backend->config.queue_capacity * sizeof(Slot) +
            backend->config.buffer_capacity * sizeof(Buffer) +
            backend->config.worker_count * sizeof(std::thread);
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status initialize(
        void* instance,
        const rtfw_device_init_config* requested) noexcept {
        auto* backend = self(instance);
        if (!backend || !requested ||
            requested->struct_size < sizeof(*requested) ||
            requested->abi_version != RTFW_DEVICE_ABI_VERSION ||
            requested->requested_in_flight == 0 ||
            requested->requested_in_flight >
                backend->config.queue_capacity ||
            requested->requested_registered_buffers >
                backend->config.buffer_capacity ||
            !words_zero(
                requested->reserved,
                std::size(requested->reserved))) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        bool expected = false;
        if (!backend->initialize_active.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        if (backend->initialized.load(std::memory_order_acquire) ||
            backend->shutdown_incomplete.load(std::memory_order_acquire)) {
            backend->initialize_active.store(
                false,
                std::memory_order_release);
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }

        auto result =
            backend->driver.initialize(backend->driver.user_data);
        if (result != XdmaDriverResult::success) {
            const auto status = normalize(result);
            backend->account_completion(status);
            backend->initialize_active.store(
                false,
                std::memory_order_release);
            return status;
        }

        backend->stop_requested.store(false, std::memory_order_release);
        backend->accepting.store(true, std::memory_order_release);
        backend->active_queue_capacity =
            static_cast<std::size_t>(requested->requested_in_flight);
        backend->active_buffer_capacity =
            static_cast<std::size_t>(
                requested->requested_registered_buffers);
        backend->created_workers = 0;
        try {
            for (std::size_t index = 0;
                 index < backend->config.worker_count;
                 ++index) {
                backend->workers[index] = std::thread(
                    [backend, index] { backend->worker_loop(index); });
                ++backend->created_workers;
            }
        } catch (...) {
            backend->accepting.store(false, std::memory_order_release);
            backend->stop_requested.store(true, std::memory_order_release);
            backend->work_epoch.fetch_add(1, std::memory_order_release);
            backend->work_epoch.notify_all();
            for (std::size_t index = 0;
                 index < backend->created_workers;
                 ++index) {
                if (backend->workers[index].joinable()) {
                    backend->workers[index].join();
                }
            }
            backend->created_workers = 0;
            backend->account_completion(
                RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED);
            const auto cleanup =
                backend->driver.shutdown(backend->driver.user_data);
            if (cleanup != XdmaDriverResult::success) {
                backend->shutdown_incomplete.store(
                    true,
                    std::memory_order_release);
                backend->account_completion(normalize(cleanup));
            }
            backend->initialize_active.store(
                false,
                std::memory_order_release);
            return RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED;
        }

        backend->last_status.store(
            RTFW_DEVICE_STATUS_OK,
            std::memory_order_release);
        backend->health_state.store(
            RTFW_DEVICE_HEALTH_HEALTHY,
            std::memory_order_release);
        backend->initialized.store(true, std::memory_order_release);
        backend->initialize_active.store(false, std::memory_order_release);
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status register_buffer(
        void* instance,
        const rtfw_device_buffer_registration* registration,
        std::uint64_t* out_token) noexcept {
        auto* backend = self(instance);
        if (out_token) {
            *out_token = 0;
        }
        if (!backend || !registration || !out_token ||
            !backend->initialized.load(std::memory_order_acquire) ||
            registration->struct_size < sizeof(*registration) ||
            !registration->data ||
            registration->bytes == 0 ||
            registration->bytes > backend->config.max_buffer_bytes ||
            registration->bytes >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max()) ||
            registration->flags == 0 ||
            (registration->flags & ~kKnownBufferFlags) != 0 ||
            !valid_identifier(
                registration->name,
                RTFW_DEVICE_IDENTIFIER_CAPACITY) ||
            !words_zero(
                registration->reserved,
                std::size(registration->reserved)) ||
            !aligned(
                static_cast<std::uint64_t>(
                    reinterpret_cast<std::uintptr_t>(
                        registration->data)),
                backend->config.transfer_alignment) ||
            !aligned(
                registration->bytes,
                backend->config.transfer_alignment)) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }

        const auto begin =
            reinterpret_cast<std::uintptr_t>(registration->data);
        if (registration->bytes >
            std::numeric_limits<std::uintptr_t>::max() - begin) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        const auto end =
            begin + static_cast<std::uintptr_t>(registration->bytes);
        std::size_t free_index = backend->active_buffer_capacity;
        for (std::size_t index = 0;
             index < backend->active_buffer_capacity;
             ++index) {
            auto& buffer = backend->buffers[index];
            if (!buffer.registered) {
                if (free_index == backend->active_buffer_capacity) {
                    free_index = index;
                }
                continue;
            }
            if (std::string_view(buffer.name.data()) ==
                std::string_view(registration->name)) {
                return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
            }
            const auto other_begin =
                reinterpret_cast<std::uintptr_t>(buffer.data);
            const auto other_end =
                other_begin + static_cast<std::uintptr_t>(buffer.bytes);
            if (begin < other_end && other_begin < end) {
                return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
            }
        }
        if (free_index == backend->active_buffer_capacity) {
            return RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED;
        }

        auto& buffer = backend->buffers[free_index];
        buffer = {};
        buffer.data = registration->data;
        buffer.bytes = registration->bytes;
        buffer.flags = registration->flags;
        buffer.token = static_cast<std::uint64_t>(free_index + 1);
        buffer.registered = true;
        std::copy(
            std::begin(registration->name),
            std::end(registration->name),
            buffer.name.begin());
        *out_token = buffer.token;
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status unregister_buffer(
        void* instance,
        std::uint64_t token) noexcept {
        auto* backend = self(instance);
        if (!backend ||
            !backend->initialized.load(std::memory_order_acquire)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        auto* buffer = backend->buffer_for(token);
        if (!buffer) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        if (backend->active_slot_references(token)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        *buffer = {};
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status submit(
        void* instance,
        const rtfw_device_submission* submission) noexcept {
        auto* backend = self(instance);
        if (!backend || !submission ||
            !backend->initialized.load(std::memory_order_acquire) ||
            !backend->accepting.load(std::memory_order_acquire)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        const auto state =
            backend->health_state.load(std::memory_order_acquire);
        if (state == RTFW_DEVICE_HEALTH_LOST) {
            return RTFW_DEVICE_STATUS_LOST;
        }
        if (state == RTFW_DEVICE_HEALTH_RESET_REQUIRED) {
            return RTFW_DEVICE_STATUS_RESET_REQUIRED;
        }

        XdmaTransfer transfer{};
        Buffer* buffer = nullptr;
        XdmaDirection direction = XdmaDirection::host_to_card;
        if (!backend->submission_valid(
                *submission,
                transfer,
                buffer,
                direction)) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }

        const auto first =
            backend->slot_hint.fetch_add(1, std::memory_order_relaxed);
        Slot* slot = nullptr;
        for (std::size_t attempt = 0;
             attempt < backend->active_queue_capacity;
             ++attempt) {
            auto& candidate = backend->slots[
                (first + attempt) %
                    backend->active_queue_capacity];
            auto expected = kSlotFree;
            if (candidate.state.compare_exchange_strong(
                    expected,
                    kSlotOwned,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                slot = &candidate;
                break;
            }
        }
        if (!slot) {
            backend->queue_rejections.fetch_add(
                1,
                std::memory_order_relaxed);
            return RTFW_DEVICE_STATUS_QUEUE_FULL;
        }

        const auto& reference = submission->buffers[0];
        slot->submission_id = submission->submission_id;
        slot->started_ns =
            backend->driver.monotonic_time_ns(
                backend->driver.user_data);
        slot->timeout_ns = submission->timeout_ns;
        slot->device_offset = transfer.device_offset;
        slot->bytes = reference.bytes;
        slot->buffer_token = reference.buffer_token;
        slot->channel = transfer.channel;
        slot->direction = direction;
        slot->host_data =
            static_cast<void*>(
                static_cast<std::byte*>(buffer->data) +
                static_cast<std::size_t>(reference.offset));
        slot->completed_ns = 0;
        slot->bytes_transferred = 0;
        slot->result = XdmaDriverResult::error;
        slot->timed_out.store(false, std::memory_order_relaxed);

        backend->submissions.fetch_add(1, std::memory_order_relaxed);
        backend->outstanding.fetch_add(1, std::memory_order_relaxed);
        slot->state.store(kSlotQueued, std::memory_order_release);
        backend->work_epoch.fetch_add(1, std::memory_order_release);
        backend->work_epoch.notify_one();
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status poll(
        void* instance,
        rtfw_device_completion* output,
        std::uint64_t output_capacity,
        std::uint64_t* out_count) noexcept {
        auto* backend = self(instance);
        if (!backend || !out_count ||
            !backend->initialized.load(std::memory_order_acquire) ||
            (output_capacity != 0 && !output)) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        *out_count = 0;
        const auto now =
            backend->driver.monotonic_time_ns(
                backend->driver.user_data);

        for (std::size_t index = 0;
             index < backend->active_queue_capacity;
             ++index) {
            auto& slot = backend->slots[index];
            const auto state =
                slot.state.load(std::memory_order_acquire);
            if ((state == kSlotQueued || state == kSlotRunning) &&
                now >= slot.started_ns &&
                now - slot.started_ns >= slot.timeout_ns) {
                slot.timed_out.store(true, std::memory_order_release);
            }
        }

        for (std::size_t index = 0;
             index < backend->active_queue_capacity &&
             *out_count < output_capacity;
             ++index) {
            auto& slot = backend->slots[index];
            auto expected = kSlotComplete;
            if (!slot.state.compare_exchange_strong(
                    expected,
                    kSlotReaping,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                continue;
            }

            auto status = normalize(slot.result);
            if (slot.timed_out.load(std::memory_order_acquire)) {
                status = RTFW_DEVICE_STATUS_TIMEOUT;
            } else if (status == RTFW_DEVICE_STATUS_OK &&
                       slot.bytes_transferred != slot.bytes) {
                status = RTFW_DEVICE_STATUS_ERROR;
            }
            auto& completion = output[*out_count];
            completion = {};
            completion.struct_size = sizeof(completion);
            completion.status = status;
            completion.submission_id = slot.submission_id;
            completion.device_timestamp_ns = slot.completed_ns;
            completion.value = slot.bytes_transferred;
            ++*out_count;

            backend->account_completion(status);
            backend->completions.fetch_add(1, std::memory_order_relaxed);
            backend->outstanding.fetch_sub(1, std::memory_order_relaxed);
            slot.state.store(kSlotFree, std::memory_order_release);
        }
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status cancel(
        void* instance,
        std::uint64_t submission_id) noexcept {
        auto* backend = self(instance);
        if (!backend ||
            !backend->initialized.load(std::memory_order_acquire) ||
            submission_id == 0) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        return RTFW_DEVICE_STATUS_UNSUPPORTED;
    }

    static rtfw_device_status get_health(
        void* instance,
        rtfw_device_health* output) noexcept {
        auto* backend = self(instance);
        if (!backend || !output ||
            output->struct_size < sizeof(*output)) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        *output = {};
        output->struct_size = sizeof(*output);
        output->state =
            backend->health_state.load(std::memory_order_acquire);
        output->last_status =
            backend->last_status.load(std::memory_order_acquire);
        output->generation =
            backend->generation.load(std::memory_order_acquire);
        output->submissions =
            backend->submissions.load(std::memory_order_acquire);
        output->completions =
            backend->completions.load(std::memory_order_acquire);
        output->queue_rejections =
            backend->queue_rejections.load(std::memory_order_acquire);
        output->timeouts =
            backend->timeouts.load(std::memory_order_acquire);
        output->errors =
            backend->errors.load(std::memory_order_acquire);
        output->losses =
            backend->losses.load(std::memory_order_acquire);
        output->cancellations = 0;
        output->resets =
            backend->resets.load(std::memory_order_acquire);
        output->outstanding =
            backend->outstanding.load(std::memory_order_acquire);
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status reset(void* instance) noexcept {
        auto* backend = self(instance);
        if (!backend ||
            !backend->initialized.load(std::memory_order_acquire)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        if (backend->health_state.load(std::memory_order_acquire) ==
            RTFW_DEVICE_HEALTH_LOST) {
            return RTFW_DEVICE_STATUS_LOST;
        }
        if (backend->outstanding.load(std::memory_order_acquire) != 0) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }

        const auto result =
            backend->driver.reset(backend->driver.user_data);
        const auto status = normalize(result);
        if (status != RTFW_DEVICE_STATUS_OK) {
            backend->account_completion(status);
            return status;
        }
        backend->generation.fetch_add(1, std::memory_order_relaxed);
        backend->resets.fetch_add(1, std::memory_order_relaxed);
        backend->last_status.store(
            RTFW_DEVICE_STATUS_OK,
            std::memory_order_release);
        backend->health_state.store(
            RTFW_DEVICE_HEALTH_HEALTHY,
            std::memory_order_release);
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status shutdown(void* instance) noexcept {
        auto* backend = self(instance);
        if (!backend) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        bool expected = false;
        if (!backend->shutdown_active.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        const bool was_initialized = backend->initialized.exchange(
            false,
            std::memory_order_acq_rel);
        if (!was_initialized &&
            !backend->shutdown_incomplete.load(
                std::memory_order_acquire)) {
            backend->shutdown_active.store(
                false,
                std::memory_order_release);
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }

        backend->shutdown_incomplete.store(
            true,
            std::memory_order_release);
        backend->accepting.store(false, std::memory_order_release);
        if (was_initialized) {
            backend->stop_requested.store(true, std::memory_order_release);
            backend->work_epoch.fetch_add(1, std::memory_order_release);
            backend->work_epoch.notify_all();
            for (std::size_t index = 0;
                 index < backend->created_workers;
                 ++index) {
                if (backend->workers[index].joinable()) {
                    backend->workers[index].join();
                }
            }
            backend->created_workers = 0;
            for (std::size_t index = 0;
                 index < backend->config.queue_capacity;
                 ++index) {
                backend->slots[index].state.store(
                    kSlotFree,
                    std::memory_order_release);
            }
            backend->outstanding.store(0, std::memory_order_release);
            for (std::size_t index = 0;
                 index < backend->config.buffer_capacity;
                 ++index) {
                backend->buffers[index] = {};
            }
        }

        const auto result =
            backend->driver.shutdown(backend->driver.user_data);
        const auto status = normalize(result);
        if (status != RTFW_DEVICE_STATUS_OK) {
            backend->account_completion(status);
            backend->shutdown_active.store(
                false,
                std::memory_order_release);
            return status;
        }

        backend->shutdown_incomplete.store(
            false,
            std::memory_order_release);
        backend->last_status.store(
            RTFW_DEVICE_STATUS_OK,
            std::memory_order_release);
        backend->health_state.store(
            RTFW_DEVICE_HEALTH_SHUTDOWN,
            std::memory_order_release);
        backend->shutdown_active.store(false, std::memory_order_release);
        return RTFW_DEVICE_STATUS_OK;
    }

    XdmaDriverApi driver;
    XdmaBackendConfig config;
    std::unique_ptr<Slot[]> slots;
    std::unique_ptr<Buffer[]> buffers;
    std::unique_ptr<std::thread[]> workers;
    std::size_t active_queue_capacity = 0;
    std::size_t active_buffer_capacity = 0;
    std::size_t created_workers = 0;
    std::atomic<bool> initialized{false};
    std::atomic<bool> accepting{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> initialize_active{false};
    std::atomic<bool> shutdown_active{false};
    std::atomic<bool> shutdown_incomplete{false};
    std::atomic<std::size_t> slot_hint{0};
    std::atomic<std::uint64_t> work_epoch{0};
    std::atomic<std::uint64_t> completion_epoch{0};
    std::atomic<std::uint32_t> health_state{
        RTFW_DEVICE_HEALTH_SHUTDOWN};
    std::atomic<rtfw_device_status> last_status{
        RTFW_DEVICE_STATUS_OK};
    std::atomic<std::uint64_t> generation{1};
    std::atomic<std::uint64_t> submissions{0};
    std::atomic<std::uint64_t> completions{0};
    std::atomic<std::uint64_t> queue_rejections{0};
    std::atomic<std::uint64_t> timeouts{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> losses{0};
    std::atomic<std::uint64_t> resets{0};
    std::atomic<std::uint64_t> outstanding{0};
};

XdmaDeviceBackend::XdmaDeviceBackend(
    const XdmaDriverApi& driver,
    const XdmaBackendConfig& config)
    : impl_(std::make_unique<Impl>(driver, config)) {}

XdmaDeviceBackend::~XdmaDeviceBackend() = default;
XdmaDeviceBackend::XdmaDeviceBackend(
    XdmaDeviceBackend&&) noexcept = default;
XdmaDeviceBackend& XdmaDeviceBackend::operator=(
    XdmaDeviceBackend&&) noexcept = default;

rtfw_device_backend_api XdmaDeviceBackend::api() noexcept {
    rtfw_device_backend_api table{};
    table.struct_size = sizeof(table);
    table.abi_version = RTFW_DEVICE_ABI_VERSION;
    table.instance = impl_.get();
    table.get_capabilities = &Impl::get_capabilities;
    table.initialize = &Impl::initialize;
    table.register_buffer = &Impl::register_buffer;
    table.unregister_buffer = &Impl::unregister_buffer;
    table.submit = &Impl::submit;
    table.poll = &Impl::poll;
    table.cancel = &Impl::cancel;
    table.get_health = &Impl::get_health;
    table.reset = &Impl::reset;
    table.shutdown = &Impl::shutdown;
    return table;
}

} // namespace rt

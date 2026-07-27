#include <rt/mock_device.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kSlotFree = 0;
constexpr std::uint32_t kSlotOwned = 1;
constexpr std::uint32_t kSlotPending = 2;
constexpr std::size_t kAbsoluteCapacityLimit = std::size_t{1} << 20;

bool bytes_zero(const std::uint64_t* values, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (values[index] != 0) {
            return false;
        }
    }
    return true;
}

bool identifier_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '.' || value == '_' || value == ':' ||
           value == '/' || value == '@' || value == '-';
}

bool valid_identifier(const char* value, std::size_t capacity) noexcept {
    if (!value || capacity == 0) {
        return false;
    }
    std::size_t length = 0;
    while (length < capacity && value[length] != '\0') {
        if (!identifier_character(value[length])) {
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

bool valid_access(std::uint32_t access) noexcept {
    return access == RTFW_DEVICE_ACCESS_READ ||
           access == RTFW_DEVICE_ACCESS_WRITE ||
           access == RTFW_DEVICE_ACCESS_READ_WRITE;
}

std::uint32_t saturating_add(
    std::uint32_t value,
    std::uint32_t increment) noexcept {
    return increment >
            std::numeric_limits<std::uint32_t>::max() - value
        ? std::numeric_limits<std::uint32_t>::max()
        : value + increment;
}

} // namespace

namespace rt {

struct MockDeviceBackend::Impl {
    struct Slot {
        std::atomic<std::uint32_t> state{kSlotFree};
        rtfw_device_submission submission{};
        std::uint64_t ordinal = 0;
        std::uint32_t poll_count = 0;
        std::uint32_t due_poll = 1;
        rtfw_device_status completion_status =
            RTFW_DEVICE_STATUS_OK;
    };

    struct Buffer {
        void* data = nullptr;
        std::uint64_t bytes = 0;
        std::uint32_t flags = 0;
        std::uint64_t token = 0;
        bool registered = false;
        std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
    };

    explicit Impl(const MockDeviceConfig& requested)
        : config(requested),
          slots(std::make_unique<Slot[]>(requested.queue_capacity)),
          buffers(std::make_unique<Buffer[]>(requested.buffer_capacity)) {
        if (config.queue_capacity == 0 ||
            config.queue_capacity > kAbsoluteCapacityLimit ||
            config.buffer_capacity == 0 ||
            config.buffer_capacity > kAbsoluteCapacityLimit ||
            config.default_completion_polls == 0 ||
            config.poll_quantum_ns == 0) {
            throw std::invalid_argument(
                "invalid deterministic mock device capacity or timing");
        }
    }

    [[nodiscard]] static Impl* self(void* instance) noexcept {
        return static_cast<Impl*>(instance);
    }

    [[nodiscard]] const MockDeviceFaultRule* fault_for(
        std::uint64_t ordinal) const noexcept {
        const auto found = std::lower_bound(
            faults.begin(),
            faults.end(),
            ordinal,
            [](const MockDeviceFaultRule& rule, std::uint64_t value) {
                return rule.submission_ordinal < value;
            });
        return found != faults.end() &&
                found->submission_ordinal == ordinal
            ? &*found
            : nullptr;
    }

    [[nodiscard]] Buffer* buffer_for(std::uint64_t token) noexcept {
        for (std::size_t index = 0;
             index < config.buffer_capacity;
             ++index) {
            auto& buffer = buffers[index];
            if (buffer.registered && buffer.token == token) {
                return &buffer;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Buffer* buffer_for(
        std::uint64_t token) const noexcept {
        return const_cast<Impl*>(this)->buffer_for(token);
    }

    [[nodiscard]] bool submission_valid(
        const rtfw_device_submission& submission) const noexcept {
        if (submission.struct_size < sizeof(submission) ||
            submission.abi_version != RTFW_DEVICE_ABI_VERSION ||
            submission.submission_id == 0 ||
            submission.timeout_ns == 0 ||
            submission.flags != 0 ||
            submission.payload_size >
                RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY ||
            submission.buffer_count >
                RTFW_DEVICE_BUFFER_REF_CAPACITY ||
            !bytes_zero(
                submission.reserved,
                std::size(submission.reserved))) {
            return false;
        }
        for (std::size_t index = 0;
             index < submission.buffer_count;
             ++index) {
            const auto& reference = submission.buffers[index];
            const auto* buffer = buffer_for(reference.buffer_token);
            if (!buffer ||
                reference.reserved0 != 0 ||
                !valid_access(reference.access) ||
                reference.offset > buffer->bytes ||
                reference.bytes > buffer->bytes - reference.offset) {
                return false;
            }
            const bool reads =
                (reference.access & RTFW_DEVICE_ACCESS_READ) != 0;
            const bool writes =
                (reference.access & RTFW_DEVICE_ACCESS_WRITE) != 0;
            if ((reads &&
                 (buffer->flags & RTFW_DEVICE_BUFFER_DEVICE_READ) == 0) ||
                (writes &&
                 (buffer->flags & RTFW_DEVICE_BUFFER_DEVICE_WRITE) == 0)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] rtfw_device_status execute(
        const rtfw_device_submission& submission) noexcept {
        if (submission.opcode == mock_device_opcode_noop) {
            return RTFW_DEVICE_STATUS_OK;
        }
        if (submission.buffer_count == 0) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        const auto& reference = submission.buffers[0];
        auto* buffer = buffer_for(reference.buffer_token);
        if (!buffer ||
            (reference.access & RTFW_DEVICE_ACCESS_WRITE) == 0) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        auto* destination =
            static_cast<std::byte*>(buffer->data) + reference.offset;
        const auto destination_size =
            static_cast<std::size_t>(reference.bytes);
        if (submission.opcode == mock_device_opcode_write_inline) {
            if (submission.payload_size > destination_size) {
                return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
            }
            std::memcpy(
                destination,
                submission.payload,
                submission.payload_size);
            return RTFW_DEVICE_STATUS_OK;
        }
        if (submission.opcode == mock_device_opcode_fill) {
            if (submission.payload_size != 1) {
                return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
            }
            std::memset(
                destination,
                submission.payload[0],
                destination_size);
            return RTFW_DEVICE_STATUS_OK;
        }
        return RTFW_DEVICE_STATUS_UNSUPPORTED;
    }

    void set_health_after(rtfw_device_status status) noexcept {
        last_status.store(status, std::memory_order_release);
        switch (status) {
        case RTFW_DEVICE_STATUS_OK:
        case RTFW_DEVICE_STATUS_CANCELED:
            break;
        case RTFW_DEVICE_STATUS_TIMEOUT:
            timeouts.fetch_add(1, std::memory_order_relaxed);
            health_state.store(
                RTFW_DEVICE_HEALTH_DEGRADED,
                std::memory_order_release);
            break;
        case RTFW_DEVICE_STATUS_LOST:
            losses.fetch_add(1, std::memory_order_relaxed);
            health_state.store(
                RTFW_DEVICE_HEALTH_RESET_REQUIRED,
                std::memory_order_release);
            break;
        default:
            errors.fetch_add(1, std::memory_order_relaxed);
            health_state.store(
                RTFW_DEVICE_HEALTH_DEGRADED,
                std::memory_order_release);
            break;
        }
    }

    static rtfw_device_status get_capabilities(
        void* instance,
        rtfw_device_capabilities* capabilities) noexcept {
        auto* backend = self(instance);
        if (!backend || !capabilities ||
            capabilities->struct_size < sizeof(*capabilities)) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        *capabilities = {};
        capabilities->struct_size = sizeof(*capabilities);
        capabilities->abi_version = RTFW_DEVICE_ABI_VERSION;
        capabilities->max_in_flight = backend->config.queue_capacity;
        capabilities->max_registered_buffers =
            backend->config.buffer_capacity;
        capabilities->max_buffer_bytes =
            std::numeric_limits<std::uint64_t>::max();
        capabilities->inline_payload_capacity =
            RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY;
        capabilities->buffer_ref_capacity =
            RTFW_DEVICE_BUFFER_REF_CAPACITY;
        capabilities->supports_cancel = 1;
        capabilities->supports_reset = 1;
        capabilities->deterministic_mock = 1;
        std::memcpy(
            capabilities->backend_id,
            "rtfw.mock.v1",
            sizeof("rtfw.mock.v1"));
        capabilities->control_storage_bytes =
            sizeof(Impl) +
            (backend->config.queue_capacity * sizeof(Slot)) +
            (backend->config.buffer_capacity * sizeof(Buffer));
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status initialize(
        void* instance,
        const rtfw_device_init_config* requested) noexcept {
        auto* backend = self(instance);
        if (!backend || !requested ||
            requested->struct_size < sizeof(*requested) ||
            requested->abi_version != RTFW_DEVICE_ABI_VERSION ||
            !bytes_zero(
                requested->reserved,
                std::size(requested->reserved)) ||
            requested->requested_in_flight >
                backend->config.queue_capacity ||
            requested->requested_registered_buffers >
                backend->config.buffer_capacity) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        bool expected = false;
        if (!backend->initialized.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        backend->accepted_ordinal.store(0, std::memory_order_relaxed);
        backend->poll_epoch.store(0, std::memory_order_relaxed);
        backend->last_status.store(
            RTFW_DEVICE_STATUS_OK,
            std::memory_order_relaxed);
        backend->health_state.store(
            RTFW_DEVICE_HEALTH_HEALTHY,
            std::memory_order_release);
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status register_buffer(
        void* instance,
        const rtfw_device_buffer_registration* registration,
        std::uint64_t* out_token) noexcept {
        auto* backend = self(instance);
        if (!backend || !registration || !out_token ||
            !backend->initialized.load(std::memory_order_acquire) ||
            registration->struct_size < sizeof(*registration) ||
            !registration->data || registration->bytes == 0 ||
            registration->flags == 0 ||
            (registration->flags &
             ~(RTFW_DEVICE_BUFFER_HOST_READ |
               RTFW_DEVICE_BUFFER_HOST_WRITE |
               RTFW_DEVICE_BUFFER_DEVICE_READ |
               RTFW_DEVICE_BUFFER_DEVICE_WRITE)) != 0 ||
            !valid_identifier(
                registration->name,
                RTFW_DEVICE_IDENTIFIER_CAPACITY) ||
            !bytes_zero(
                registration->reserved,
                std::size(registration->reserved))) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        *out_token = 0;
        for (std::size_t index = 0;
             index < backend->config.buffer_capacity;
             ++index) {
            auto& buffer = backend->buffers[index];
            if (buffer.registered) {
                continue;
            }
            buffer.data = registration->data;
            buffer.bytes = registration->bytes;
            buffer.flags = registration->flags;
            buffer.token = static_cast<std::uint64_t>(index + 1);
            buffer.registered = true;
            std::copy_n(
                registration->name,
                RTFW_DEVICE_IDENTIFIER_CAPACITY,
                buffer.name.begin());
            *out_token = buffer.token;
            return RTFW_DEVICE_STATUS_OK;
        }
        return RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED;
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
        for (std::size_t slot_index = 0;
             slot_index < backend->config.queue_capacity;
             ++slot_index) {
            const auto& slot = backend->slots[slot_index];
            if (slot.state.load(std::memory_order_acquire) !=
                kSlotPending) {
                continue;
            }
            for (std::size_t reference_index = 0;
                 reference_index < slot.submission.buffer_count;
                 ++reference_index) {
                if (slot.submission.buffers[reference_index].buffer_token ==
                    token) {
                    return RTFW_DEVICE_STATUS_INVALID_STATE;
                }
            }
        }
        *buffer = {};
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status submit(
        void* instance,
        const rtfw_device_submission* submission) noexcept {
        auto* backend = self(instance);
        if (!backend || !submission ||
            !backend->initialized.load(std::memory_order_acquire)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        if (backend->health_state.load(std::memory_order_acquire) ==
            RTFW_DEVICE_HEALTH_RESET_REQUIRED) {
            return RTFW_DEVICE_STATUS_RESET_REQUIRED;
        }
        if (!backend->submission_valid(*submission)) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }

        const auto first =
            backend->slot_hint.fetch_add(
                1,
                std::memory_order_relaxed) %
            backend->config.queue_capacity;
        for (std::size_t offset = 0;
             offset < backend->config.queue_capacity;
             ++offset) {
            auto& slot = backend->slots[
                (first + offset) % backend->config.queue_capacity];
            auto expected = kSlotFree;
            if (!slot.state.compare_exchange_strong(
                    expected,
                    kSlotOwned,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                continue;
            }

            slot.submission = *submission;
            slot.ordinal =
                backend->accepted_ordinal.fetch_add(
                    1,
                    std::memory_order_relaxed) + 1;
            slot.poll_count = 0;
            slot.due_poll =
                backend->config.default_completion_polls;
            slot.completion_status = RTFW_DEVICE_STATUS_OK;
            if (const auto* fault =
                    backend->fault_for(slot.ordinal)) {
                switch (fault->fault) {
                case MockDeviceFault::none:
                    break;
                case MockDeviceFault::delay:
                    slot.due_poll = saturating_add(
                        slot.due_poll,
                        fault->extra_poll_delay);
                    break;
                case MockDeviceFault::timeout: {
                    const auto quotient =
                        submission->timeout_ns /
                        backend->config.poll_quantum_ns;
                    const auto remainder =
                        submission->timeout_ns %
                        backend->config.poll_quantum_ns;
                    const auto timeout_polls =
                        quotient + (remainder != 0 ? 1u : 0u);
                    const auto bounded = std::min<std::uint64_t>(
                        timeout_polls,
                        std::numeric_limits<std::uint32_t>::max());
                    slot.due_poll = std::max<std::uint32_t>(
                        1,
                        static_cast<std::uint32_t>(bounded));
                    slot.due_poll = saturating_add(
                        slot.due_poll,
                        fault->extra_poll_delay);
                    slot.completion_status =
                        RTFW_DEVICE_STATUS_TIMEOUT;
                    break;
                }
                case MockDeviceFault::error:
                    slot.due_poll = saturating_add(
                        slot.due_poll,
                        fault->extra_poll_delay);
                    slot.completion_status =
                        RTFW_DEVICE_STATUS_ERROR;
                    break;
                case MockDeviceFault::loss:
                    slot.due_poll = saturating_add(
                        slot.due_poll,
                        fault->extra_poll_delay);
                    slot.completion_status =
                        RTFW_DEVICE_STATUS_LOST;
                    break;
                }
            }
            backend->submissions.fetch_add(
                1,
                std::memory_order_relaxed);
            backend->outstanding.fetch_add(
                1,
                std::memory_order_relaxed);
            slot.state.store(kSlotPending, std::memory_order_release);
            return RTFW_DEVICE_STATUS_OK;
        }
        backend->queue_rejections.fetch_add(
            1,
            std::memory_order_relaxed);
        return RTFW_DEVICE_STATUS_QUEUE_FULL;
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
        const auto epoch =
            backend->poll_epoch.fetch_add(
                1,
                std::memory_order_relaxed) + 1;
        for (std::size_t index = 0;
             index < backend->config.queue_capacity &&
             *out_count < output_capacity;
             ++index) {
            auto& slot = backend->slots[index];
            auto expected = kSlotPending;
            if (!slot.state.compare_exchange_strong(
                    expected,
                    kSlotOwned,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                continue;
            }
            ++slot.poll_count;
            if (slot.poll_count < slot.due_poll) {
                slot.state.store(
                    kSlotPending,
                    std::memory_order_release);
                continue;
            }

            auto status = slot.completion_status;
            if (status == RTFW_DEVICE_STATUS_OK) {
                status = backend->execute(slot.submission);
            }
            auto& completion = output[*out_count];
            completion = {};
            completion.struct_size = sizeof(completion);
            completion.status = status;
            completion.submission_id =
                slot.submission.submission_id;
            completion.device_timestamp_ns =
                epoch * backend->config.poll_quantum_ns;
            completion.value = slot.ordinal;
            ++*out_count;

            backend->set_health_after(status);
            backend->completions.fetch_add(
                1,
                std::memory_order_relaxed);
            backend->outstanding.fetch_sub(
                1,
                std::memory_order_relaxed);
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
        for (std::size_t index = 0;
             index < backend->config.queue_capacity;
             ++index) {
            auto& slot = backend->slots[index];
            auto expected = kSlotPending;
            if (!slot.state.compare_exchange_strong(
                    expected,
                    kSlotOwned,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                continue;
            }
            if (slot.submission.submission_id != submission_id) {
                slot.state.store(
                    kSlotPending,
                    std::memory_order_release);
                continue;
            }
            slot.completion_status =
                RTFW_DEVICE_STATUS_CANCELED;
            slot.due_poll = 1;
            slot.poll_count = 0;
            backend->cancellations.fetch_add(
                1,
                std::memory_order_relaxed);
            slot.state.store(kSlotPending, std::memory_order_release);
            return RTFW_DEVICE_STATUS_OK;
        }
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
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
        output->state = backend->health_state.load(
            std::memory_order_acquire);
        output->last_status = backend->last_status.load(
            std::memory_order_acquire);
        output->generation = backend->generation.load(
            std::memory_order_acquire);
        output->submissions = backend->submissions.load(
            std::memory_order_acquire);
        output->completions = backend->completions.load(
            std::memory_order_acquire);
        output->queue_rejections = backend->queue_rejections.load(
            std::memory_order_acquire);
        output->timeouts = backend->timeouts.load(
            std::memory_order_acquire);
        output->errors = backend->errors.load(
            std::memory_order_acquire);
        output->losses = backend->losses.load(
            std::memory_order_acquire);
        output->cancellations = backend->cancellations.load(
            std::memory_order_acquire);
        output->resets = backend->resets.load(
            std::memory_order_acquire);
        output->outstanding = backend->outstanding.load(
            std::memory_order_acquire);
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status reset(void* instance) noexcept {
        auto* backend = self(instance);
        if (!backend ||
            !backend->initialized.load(std::memory_order_acquire)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        for (std::size_t index = 0;
             index < backend->config.queue_capacity;
             ++index) {
            auto& slot = backend->slots[index];
            const auto previous =
                slot.state.exchange(
                    kSlotFree,
                    std::memory_order_acq_rel);
            if (previous != kSlotFree) {
                backend->outstanding.fetch_sub(
                    1,
                    std::memory_order_relaxed);
            }
        }
        backend->generation.fetch_add(1, std::memory_order_relaxed);
        backend->resets.fetch_add(1, std::memory_order_relaxed);
        backend->last_status.store(
            RTFW_DEVICE_STATUS_OK,
            std::memory_order_relaxed);
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
        if (!backend->initialized.exchange(
                false,
                std::memory_order_acq_rel)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        for (std::size_t index = 0;
             index < backend->config.queue_capacity;
             ++index) {
            backend->slots[index].state.store(
                kSlotFree,
                std::memory_order_release);
        }
        backend->outstanding.store(0, std::memory_order_relaxed);
        for (std::size_t index = 0;
             index < backend->config.buffer_capacity;
             ++index) {
            backend->buffers[index] = {};
        }
        backend->health_state.store(
            RTFW_DEVICE_HEALTH_SHUTDOWN,
            std::memory_order_release);
        return RTFW_DEVICE_STATUS_OK;
    }

    MockDeviceConfig config;
    std::unique_ptr<Slot[]> slots;
    std::unique_ptr<Buffer[]> buffers;
    std::vector<MockDeviceFaultRule> faults;
    std::atomic<bool> initialized{false};
    std::atomic<std::size_t> slot_hint{0};
    std::atomic<std::uint64_t> accepted_ordinal{0};
    std::atomic<std::uint64_t> poll_epoch{0};
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
    std::atomic<std::uint64_t> cancellations{0};
    std::atomic<std::uint64_t> resets{0};
    std::atomic<std::uint64_t> outstanding{0};
};

MockDeviceBackend::MockDeviceBackend(
    const MockDeviceConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

MockDeviceBackend::~MockDeviceBackend() = default;
MockDeviceBackend::MockDeviceBackend(MockDeviceBackend&&) noexcept = default;
MockDeviceBackend& MockDeviceBackend::operator=(
    MockDeviceBackend&&) noexcept = default;

rtfw_device_backend_api MockDeviceBackend::api() noexcept {
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

rtfw_device_status MockDeviceBackend::set_fault_script(
    std::span<const MockDeviceFaultRule> rules) noexcept {
    if (!impl_ ||
        impl_->initialized.load(std::memory_order_acquire)) {
        return RTFW_DEVICE_STATUS_INVALID_STATE;
    }
    for (std::size_t index = 0; index < rules.size(); ++index) {
        if (rules[index].submission_ordinal == 0 ||
            static_cast<std::uint8_t>(rules[index].fault) >
                static_cast<std::uint8_t>(MockDeviceFault::loss) ||
            (index != 0 &&
             rules[index - 1].submission_ordinal >=
                 rules[index].submission_ordinal)) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
    }
    try {
        impl_->faults.assign(rules.begin(), rules.end());
        return RTFW_DEVICE_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED;
    } catch (...) {
        return RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    }
}

} // namespace rt

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
constexpr std::uint32_t kPathNone = 0;
constexpr std::uint32_t kPathDeviceV1 = 1;
constexpr std::uint32_t kPathNativeV2 = 2;
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

bool bytes_zero(const std::uint8_t* values, std::size_t count) noexcept {
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
    const bool prefix_valid =
           driver.struct_size >= rt::xdma_driver_api_v1_size &&
           driver.struct_size <= sizeof(driver) &&
           driver.initialize &&
           driver.transfer &&
           driver.reset &&
           driver.shutdown &&
           driver.monotonic_time_ns &&
           words_zero(driver.reserved, std::size(driver.reserved));
    if (!prefix_valid) {
        return false;
    }
    if (driver.api_version == rt::xdma_driver_api_version_1) {
        if (driver.struct_size == rt::xdma_driver_api_v1_size) {
            return true;
        }
        return driver.struct_size == sizeof(driver) &&
               driver.control_read32 == nullptr &&
               driver.control_write32 == nullptr &&
               driver.wait_user_event == nullptr &&
               driver.request_stop == nullptr &&
               words_zero(driver.reserved_v2, std::size(driver.reserved_v2));
    }
    return driver.api_version == rt::xdma_driver_api_version_2 &&
           driver.struct_size == sizeof(driver) &&
           words_zero(driver.reserved_v2, std::size(driver.reserved_v2));
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
        config.transfer_alignment > config.max_transfer_bytes ||
        config.control_aperture_bytes >
            rt::xdma_control_aperture_max_bytes ||
        (config.control_aperture_bytes != 0 &&
         ((config.control_aperture_bytes & 3u) != 0 ||
          driver.api_version != rt::xdma_driver_api_version_2 ||
          !driver.control_read32 || !driver.control_write32)) ||
        config.user_event_count > rt::xdma_user_event_capacity ||
        (config.user_event_count != 0 &&
         (driver.api_version != rt::xdma_driver_api_version_2 ||
          !driver.wait_user_event || !driver.request_stop)) ||
        (driver.api_version == rt::xdma_driver_api_version_1 &&
         (config.control_aperture_bytes != 0 ||
          config.user_event_count != 0))) {
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

rt::HalV2Status normalize_hal(rt::XdmaDriverResult result) noexcept {
    return static_cast<rt::HalV2Status>(normalize(result));
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

    struct BatchSlot {
        std::atomic<std::uint32_t> state{kSlotFree};
        std::atomic<bool> canceled{false};
        std::atomic<bool> active_event_wait{false};
        std::uint64_t sequence = 0;
        std::uint64_t started_ns = 0;
        DeviceCommandBatch batch{};
        HalV2BatchCompletion completion{};
    };

    Impl(
        const XdmaDriverApi& requested_driver,
        const XdmaBackendConfig& requested_config)
        : driver(requested_driver),
          config(validated_config(requested_driver, requested_config)),
          slots(std::make_unique<Slot[]>(config.queue_capacity)),
          batch_slots(std::make_unique<BatchSlot[]>(config.queue_capacity)),
          buffers(std::make_unique<Buffer[]>(config.buffer_capacity)),
          workers(std::make_unique<std::thread[]>(config.worker_count)) {
        initialize_hal_tables();
    }

    ~Impl() {
        if (initialized.load(std::memory_order_acquire) ||
            shutdown_incomplete.load(std::memory_order_acquire)) {
            (void)shutdown_common(
                this, registration_path.load(std::memory_order_acquire));
        }
    }

    static Impl* self(void* instance) noexcept {
        return static_cast<Impl*>(instance);
    }

    void initialize_hal_tables() noexcept;

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
        case RTFW_DEVICE_STATUS_CANCELED:
            cancellations.fetch_add(1, std::memory_order_relaxed);
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
        for (std::size_t index = 0; index < active_queue_capacity; ++index) {
            const auto state =
                batch_slots[index].state.load(std::memory_order_acquire);
            if (state == kSlotFree) {
                continue;
            }
            const auto& batch = batch_slots[index].batch;
            for (std::size_t command_index = 0;
                 command_index < batch.command_count; ++command_index) {
                const auto& command = batch.commands[command_index];
                for (std::size_t ref_index = 0;
                     ref_index < command.buffer_count; ++ref_index) {
                    if (command.buffers[ref_index].buffer_token == token) {
                        return true;
                    }
                }
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
        for (std::size_t index = 0; index < active_queue_capacity; ++index) {
            const auto state =
                batch_slots[index].state.load(std::memory_order_acquire);
            if (state == kSlotOwned || state == kSlotQueued ||
                state == kSlotRunning) {
                return true;
            }
        }
        return false;
    }

    bool validate_batch_reference(
        const HalV2BufferReference& reference,
        std::uint32_t required_access,
        Buffer*& buffer,
        void*& address) noexcept {
        buffer = buffer_for(reference.buffer_token);
        if (!buffer || reference.reserved0 != 0 ||
            reference.access != required_access || reference.bytes == 0 ||
            reference.offset > buffer->bytes ||
            reference.bytes > buffer->bytes - reference.offset) {
            return false;
        }
        const auto required_flag = required_access == RTFW_DEVICE_ACCESS_READ
            ? RTFW_DEVICE_BUFFER_HOST_READ
            : RTFW_DEVICE_BUFFER_HOST_WRITE;
        if ((buffer->flags & required_flag) == 0 ||
            reference.offset >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
            return false;
        }
        address = static_cast<std::byte*>(buffer->data) +
            static_cast<std::size_t>(reference.offset);
        return true;
    }

    bool validate_batch_command(const DeviceCommand& command) noexcept {
        if (command.struct_size != sizeof(command) ||
            command.extension_version !=
                hal_v2_command_timeline_extension_version ||
            command.kind !=
                static_cast<std::uint32_t>(HalV2CommandKind::dispatch) ||
            command.operation !=
                static_cast<std::uint32_t>(HalV2MemoryOperation::invalid) ||
            command.flags != 0 ||
            command.payload_size > command.payload.size() ||
            command.buffer_count > command.buffers.size() ||
            !words_zero(command.reserved.data(), command.reserved.size())) {
            return false;
        }
        if (!bytes_zero(
                command.payload.data() + command.payload_size,
                command.payload.size() - command.payload_size)) {
            return false;
        }
        const auto inactive_reference = [](const HalV2BufferReference& ref) {
            return ref.buffer_token == 0 && ref.access == 0 &&
                   ref.reserved0 == 0 && ref.offset == 0 && ref.bytes == 0;
        };
        if (!inactive_reference(command.source) ||
            !inactive_reference(command.destination) ||
            !inactive_reference(command.target)) {
            return false;
        }
        for (std::size_t index = command.buffer_count;
             index < command.buffers.size(); ++index) {
            if (!inactive_reference(command.buffers[index])) {
                return false;
            }
        }

        Buffer* buffer = nullptr;
        void* address = nullptr;
        if (command.opcode == xdma_device_opcode_host_to_card ||
            command.opcode == xdma_device_opcode_card_to_host) {
            if (command.payload_size != sizeof(XdmaTransfer) ||
                command.buffer_count != 1) {
                return false;
            }
            XdmaTransfer transfer{};
            std::memcpy(&transfer, command.payload.data(), sizeof(transfer));
            if (transfer.reserved0 != 0 ||
                !words_zero(transfer.reserved, std::size(transfer.reserved))) {
                return false;
            }
            const bool h2c =
                command.opcode == xdma_device_opcode_host_to_card;
            if ((h2c && transfer.channel >= config.h2c_channel_count) ||
                (!h2c && transfer.channel >= config.c2h_channel_count) ||
                !validate_batch_reference(
                    command.buffers[0],
                    h2c ? RTFW_DEVICE_ACCESS_READ
                        : RTFW_DEVICE_ACCESS_WRITE,
                    buffer,
                    address) ||
                command.buffers[0].bytes > config.max_transfer_bytes ||
                transfer.device_offset >
                    std::numeric_limits<std::uint64_t>::max() -
                        command.buffers[0].bytes ||
                !aligned(command.buffers[0].offset, config.transfer_alignment) ||
                !aligned(command.buffers[0].bytes, config.transfer_alignment) ||
                !aligned(transfer.device_offset, config.transfer_alignment) ||
                !aligned(
                    static_cast<std::uint64_t>(
                        reinterpret_cast<std::uintptr_t>(address)),
                    config.transfer_alignment)) {
                return false;
            }
            const auto required_device_flag = h2c
                ? RTFW_DEVICE_BUFFER_DEVICE_WRITE
                : RTFW_DEVICE_BUFFER_DEVICE_READ;
            return (buffer->flags & required_device_flag) != 0;
        }

        const auto family = command.opcode & 0xffff'0000u;
        const auto selector = command.opcode & 0x0000'ffffu;
        if (family == xdma_device_opcode_control_read_base) {
            const auto offset = selector * 4u;
            return driver.api_version == xdma_driver_api_version_2 &&
                   driver.control_read32 &&
                   config.control_aperture_bytes != 0 &&
                   offset < config.control_aperture_bytes &&
                   command.payload_size == 0 && command.buffer_count == 1 &&
                   command.buffers[0].bytes == sizeof(std::uint32_t) &&
                   validate_batch_reference(
                       command.buffers[0], RTFW_DEVICE_ACCESS_WRITE,
                       buffer, address);
        }
        if (family == xdma_device_opcode_control_write_base) {
            const auto offset = selector * 4u;
            return driver.api_version == xdma_driver_api_version_2 &&
                   driver.control_write32 &&
                   config.control_aperture_bytes != 0 &&
                   offset < config.control_aperture_bytes &&
                   command.payload_size == sizeof(std::uint32_t) &&
                   command.buffer_count == 0;
        }
        if (family == xdma_device_opcode_user_event_base) {
            return driver.api_version == xdma_driver_api_version_2 &&
                   driver.wait_user_event && driver.request_stop &&
                   selector < config.user_event_count &&
                   command.payload_size == 0 && command.buffer_count == 1 &&
                   command.buffers[0].bytes == sizeof(std::uint32_t) &&
                   validate_batch_reference(
                       command.buffers[0], RTFW_DEVICE_ACCESS_WRITE,
                       buffer, address);
        }
        return false;
    }

    bool batch_valid(const DeviceCommandBatch& batch) noexcept {
        if (batch.struct_size != sizeof(batch) ||
            batch.extension_version !=
                hal_v2_command_timeline_extension_version ||
            batch.batch_id == 0 || batch.timeout_ns == 0 ||
            batch.command_count == 0 ||
            batch.command_count > hal_v2_command_capacity ||
            batch.wait_count > hal_v2_timeline_wait_capacity ||
            batch.signal_count == 0 ||
            batch.signal_count > hal_v2_timeline_signal_capacity ||
            batch.reserved0 != 0 ||
            !words_zero(batch.reserved.data(), batch.reserved.size())) {
            return false;
        }
        for (std::size_t index = 0; index < batch.command_count; ++index) {
            if (!validate_batch_command(batch.commands[index])) {
                return false;
            }
        }
        for (std::size_t index = batch.command_count;
             index < batch.commands.size(); ++index) {
            const auto& command = batch.commands[index];
            if (command.struct_size != sizeof(command) ||
                command.extension_version !=
                    hal_v2_command_timeline_extension_version ||
                command.kind !=
                    static_cast<std::uint32_t>(HalV2CommandKind::invalid) ||
                command.operation != static_cast<std::uint32_t>(
                    HalV2MemoryOperation::invalid) ||
                command.opcode != 0 || command.flags != 0 ||
                command.payload_size != 0 || command.buffer_count != 0 ||
                !bytes_zero(command.payload.data(), command.payload.size()) ||
                command.source.buffer_token != 0 ||
                command.destination.buffer_token != 0 ||
                command.target.buffer_token != 0 ||
                !words_zero(command.reserved.data(), command.reserved.size())) {
                return false;
            }
            for (const auto& reference : command.buffers) {
                if (reference.buffer_token != 0 || reference.access != 0 ||
                    reference.reserved0 != 0 || reference.offset != 0 ||
                    reference.bytes != 0) {
                    return false;
                }
            }
        }
        for (std::size_t index = 0; index < batch.wait_count; ++index) {
            const auto& point = batch.waits[index];
            if (point.struct_size != sizeof(point) ||
                point.extension_version !=
                    hal_v2_command_timeline_extension_version ||
                point.timeline_handle == invalid_device_handle ||
                point.value == 0 ||
                !words_zero(point.reserved.data(), point.reserved.size())) {
                return false;
            }
        }
        for (std::size_t index = 0; index < batch.signal_count; ++index) {
            const auto& point = batch.signals[index];
            if (point.struct_size != sizeof(point) ||
                point.extension_version !=
                    hal_v2_command_timeline_extension_version ||
                point.timeline_handle == invalid_device_handle ||
                point.value == 0 ||
                !words_zero(point.reserved.data(), point.reserved.size())) {
                return false;
            }
        }
        const auto inactive_point = [](const HalV2TimelinePoint& point) {
            return point.struct_size == sizeof(point) &&
                   point.extension_version ==
                       hal_v2_command_timeline_extension_version &&
                   point.timeline_handle == invalid_device_handle &&
                   point.value == 0 &&
                   words_zero(point.reserved.data(), point.reserved.size());
        };
        for (std::size_t index = batch.wait_count;
             index < batch.waits.size(); ++index) {
            if (!inactive_point(batch.waits[index])) {
                return false;
            }
        }
        for (std::size_t index = batch.signal_count;
             index < batch.signals.size(); ++index) {
            if (!inactive_point(batch.signals[index])) {
                return false;
            }
        }
        return true;
    }

    XdmaDriverResult execute_batch_command(
        const DeviceCommand& command,
        std::uint64_t deadline_ns,
        std::uint64_t& value) noexcept {
        if (command.opcode == xdma_device_opcode_host_to_card ||
            command.opcode == xdma_device_opcode_card_to_host) {
            XdmaTransfer transfer{};
            std::memcpy(&transfer, command.payload.data(), sizeof(transfer));
            const auto& reference = command.buffers[0];
            auto* buffer = buffer_for(reference.buffer_token);
            auto* address = static_cast<std::byte*>(buffer->data) +
                static_cast<std::size_t>(reference.offset);
            const auto result = driver.transfer(
                driver.user_data,
                command.opcode == xdma_device_opcode_host_to_card
                    ? XdmaDirection::host_to_card
                    : XdmaDirection::card_to_host,
                transfer.channel, transfer.device_offset, address,
                reference.bytes);
            value = result.bytes_transferred;
            return result.result == XdmaDriverResult::success &&
                    result.bytes_transferred != reference.bytes
                ? XdmaDriverResult::io_error
                : result.result;
        }
        const auto family = command.opcode & 0xffff'0000u;
        const auto selector = command.opcode & 0x0000'ffffu;
        if (family == xdma_device_opcode_control_read_base) {
            const auto result = driver.control_read32(
                driver.user_data, selector * 4u);
            if (result.result == XdmaDriverResult::success) {
                const auto& reference = command.buffers[0];
                auto* buffer = buffer_for(reference.buffer_token);
                auto* output = static_cast<std::uint8_t*>(buffer->data) +
                    static_cast<std::size_t>(reference.offset);
                output[0] = static_cast<std::uint8_t>(result.value);
                output[1] = static_cast<std::uint8_t>(result.value >> 8u);
                output[2] = static_cast<std::uint8_t>(result.value >> 16u);
                output[3] = static_cast<std::uint8_t>(result.value >> 24u);
                value = result.value;
            }
            return result.result;
        }
        if (family == xdma_device_opcode_control_write_base) {
            const auto value32 =
                static_cast<std::uint32_t>(command.payload[0]) |
                (static_cast<std::uint32_t>(command.payload[1]) << 8u) |
                (static_cast<std::uint32_t>(command.payload[2]) << 16u) |
                (static_cast<std::uint32_t>(command.payload[3]) << 24u);
            value = value32;
            return driver.control_write32(
                driver.user_data, selector * 4u, value32);
        }
        const auto now = driver.monotonic_time_ns(driver.user_data);
        if (now >= deadline_ns) {
            return XdmaDriverResult::timeout;
        }
        const auto result = driver.wait_user_event(
            driver.user_data, selector, deadline_ns - now);
        if (result.result == XdmaDriverResult::success) {
            const auto& reference = command.buffers[0];
            auto* buffer = buffer_for(reference.buffer_token);
            auto* output = static_cast<std::uint8_t*>(buffer->data) +
                static_cast<std::size_t>(reference.offset);
            output[0] = static_cast<std::uint8_t>(result.value);
            output[1] = static_cast<std::uint8_t>(result.value >> 8u);
            output[2] = static_cast<std::uint8_t>(result.value >> 16u);
            output[3] = static_cast<std::uint8_t>(result.value >> 24u);
            value = result.value;
        }
        return result.result;
    }

    bool claim_and_execute_batch() noexcept {
        BatchSlot* chosen = nullptr;
        std::uint64_t sequence = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t index = 0; index < active_queue_capacity; ++index) {
            auto& candidate = batch_slots[index];
            if (candidate.state.load(std::memory_order_acquire) == kSlotQueued &&
                candidate.sequence < sequence) {
                chosen = &candidate;
                sequence = candidate.sequence;
            }
        }
        if (!chosen) {
            return false;
        }
        auto expected = kSlotQueued;
        if (!chosen->state.compare_exchange_strong(
                expected, kSlotRunning, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return true;
        }
        const auto deadline = chosen->started_ns + chosen->batch.timeout_ns;
        auto result = XdmaDriverResult::success;
        std::uint64_t value = 0;
        for (std::size_t index = 0;
             index < chosen->batch.command_count; ++index) {
            const auto family =
                chosen->batch.commands[index].opcode & 0xffff'0000u;
            const bool event_wait =
                family == xdma_device_opcode_user_event_base;
            chosen->active_event_wait.store(
                event_wait, std::memory_order_release);
            if (chosen->canceled.load(std::memory_order_acquire)) {
                result = XdmaDriverResult::error;
                chosen->active_event_wait.store(
                    false, std::memory_order_release);
                break;
            }
            const auto now = driver.monotonic_time_ns(driver.user_data);
            if (now >= deadline) {
                result = XdmaDriverResult::timeout;
                chosen->active_event_wait.store(
                    false, std::memory_order_release);
                break;
            }
            result = execute_batch_command(
                chosen->batch.commands[index], deadline, value);
            chosen->active_event_wait.store(
                false, std::memory_order_release);
            if (result != XdmaDriverResult::success) {
                break;
            }
        }
        auto status = normalize_hal(result);
        if (chosen->canceled.load(std::memory_order_acquire)) {
            status = HalV2Status::canceled;
        } else if (status == HalV2Status::ok &&
                   driver.monotonic_time_ns(driver.user_data) >= deadline) {
            status = HalV2Status::timeout;
        }
        chosen->completion = {};
        chosen->completion.status = static_cast<std::int32_t>(status);
        chosen->completion.batch_id = chosen->batch.batch_id;
        chosen->completion.device_timestamp =
            driver.monotonic_time_ns(driver.user_data);
        chosen->completion.timestamp_domain_identity = 1;
        chosen->completion.signal_count = chosen->batch.signal_count;
        for (std::size_t index = 0;
             index < chosen->batch.signal_count; ++index) {
            chosen->completion.signals[index] = chosen->batch.signals[index];
        }
        chosen->state.store(kSlotComplete, std::memory_order_release);
        completion_epoch.fetch_add(1, std::memory_order_release);
        completion_epoch.notify_all();
        return true;
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
            if (worker_index == 0 && claim_and_execute_batch()) {
                observed = work_epoch.load(std::memory_order_acquire);
                continue;
            }
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

    static rtfw_device_status initialize_common(
        void* instance,
        const rtfw_device_init_config* requested,
        std::uint32_t requested_path) noexcept {
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
        auto expected_path = kPathNone;
        if (!backend->registration_path.compare_exchange_strong(
                expected_path, requested_path, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        bool expected = false;
        if (!backend->initialize_active.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            backend->registration_path.store(kPathNone, std::memory_order_release);
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        if (backend->initialized.load(std::memory_order_acquire) ||
            backend->shutdown_incomplete.load(std::memory_order_acquire)) {
            backend->initialize_active.store(
                false,
                std::memory_order_release);
            backend->registration_path.store(kPathNone, std::memory_order_release);
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }

        auto result =
            backend->driver.initialize(backend->driver.user_data);
        if (result != XdmaDriverResult::success) {
            const auto status = normalize(result);
            backend->account_completion(status);
            const auto cleanup =
                backend->driver.shutdown(backend->driver.user_data);
            if (cleanup != XdmaDriverResult::success &&
                cleanup != XdmaDriverResult::invalid_value) {
                backend->shutdown_incomplete.store(
                    true,
                    std::memory_order_release);
                backend->account_completion(normalize(cleanup));
            } else {
                backend->registration_path.store(
                    kPathNone, std::memory_order_release);
            }
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
            } else {
                backend->registration_path.store(
                    kPathNone, std::memory_order_release);
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

    static rtfw_device_status initialize_v1(
        void* instance,
        const rtfw_device_init_config* requested) noexcept {
        return initialize_common(instance, requested, kPathDeviceV1);
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
        output->cancellations =
            backend->cancellations.load(std::memory_order_acquire);
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
        for (std::size_t index = 0;
             index < backend->active_queue_capacity; ++index) {
            if (backend->batch_slots[index].state.load(
                    std::memory_order_acquire) != kSlotFree) {
                return RTFW_DEVICE_STATUS_INVALID_STATE;
            }
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

    static rtfw_device_status shutdown_common(
        void* instance,
        std::uint32_t requested_path) noexcept {
        auto* backend = self(instance);
        if (!backend || requested_path == kPathNone) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        const auto active_path =
            backend->registration_path.load(std::memory_order_acquire);
        if (active_path == kPathNone) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        if (active_path != requested_path) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
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
            if (requested_path == kPathNativeV2 &&
                backend->driver.request_stop) {
                (void)backend->driver.request_stop(backend->driver.user_data);
            }
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
                backend->batch_slots[index].state.store(
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
        backend->registration_path.store(kPathNone, std::memory_order_release);
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status shutdown_v1(void* instance) noexcept {
        return shutdown_common(instance, kPathDeviceV1);
    }

    static bool path_is(void* instance, std::uint32_t path) noexcept {
        auto* backend = self(instance);
        return backend && backend->registration_path.load(
            std::memory_order_acquire) == path;
    }

    static rtfw_device_status register_buffer_v1(
        void* instance,
        const rtfw_device_buffer_registration* registration,
        std::uint64_t* token) noexcept {
        return path_is(instance, kPathDeviceV1)
            ? register_buffer(instance, registration, token)
            : RTFW_DEVICE_STATUS_INVALID_STATE;
    }

    static rtfw_device_status unregister_buffer_v1(
        void* instance,
        std::uint64_t token) noexcept {
        return path_is(instance, kPathDeviceV1)
            ? unregister_buffer(instance, token)
            : RTFW_DEVICE_STATUS_INVALID_STATE;
    }

    static rtfw_device_status submit_v1(
        void* instance,
        const rtfw_device_submission* submission) noexcept {
        return path_is(instance, kPathDeviceV1)
            ? submit(instance, submission)
            : RTFW_DEVICE_STATUS_INVALID_STATE;
    }

    static rtfw_device_status poll_v1(
        void* instance,
        rtfw_device_completion* output,
        std::uint64_t capacity,
        std::uint64_t* count) noexcept {
        return path_is(instance, kPathDeviceV1)
            ? poll(instance, output, capacity, count)
            : RTFW_DEVICE_STATUS_INVALID_STATE;
    }

    static rtfw_device_status cancel_v1(
        void* instance,
        std::uint64_t submission_id) noexcept {
        return path_is(instance, kPathDeviceV1)
            ? cancel(instance, submission_id)
            : RTFW_DEVICE_STATUS_INVALID_STATE;
    }

    static rtfw_device_status get_health_v1(
        void* instance,
        rtfw_device_health* output) noexcept {
        return path_is(instance, kPathDeviceV1)
            ? get_health(instance, output)
            : RTFW_DEVICE_STATUS_INVALID_STATE;
    }

    static rtfw_device_status reset_v1(void* instance) noexcept {
        return path_is(instance, kPathDeviceV1)
            ? reset(instance)
            : RTFW_DEVICE_STATUS_INVALID_STATE;
    }

    static HalV2Status hal_get_capabilities(
        void* instance,
        HalV2Capabilities* output) noexcept {
        auto* backend = self(instance);
        if (!backend || !output ||
            output->struct_size < sizeof(*output) ||
            backend->driver.api_version != xdma_driver_api_version_2) {
            return HalV2Status::invalid_argument;
        }
        *output = {};
        output->max_in_flight = backend->config.queue_capacity;
        output->max_registered_buffers = backend->config.buffer_capacity;
        output->max_buffer_bytes = backend->config.max_buffer_bytes;
        output->inline_payload_capacity = hal_v2_inline_payload_capacity;
        output->buffer_ref_capacity = hal_v2_buffer_ref_capacity;
        output->supports_reset = 1;
        constexpr std::string_view identifier =
            "rtfw.xdma.xilinx_linux_aximm.v2";
        std::copy(identifier.begin(), identifier.end(), output->backend_id.begin());
        output->control_storage_bytes =
            backend->config.queue_capacity *
                (sizeof(Slot) + sizeof(BatchSlot)) +
            backend->config.buffer_capacity * sizeof(Buffer) +
            backend->config.worker_count * sizeof(std::thread);
        return HalV2Status::ok;
    }

    static HalV2Status hal_initialize(
        void* instance,
        const HalV2InitializeConfig* requested) noexcept {
        if (!requested || requested->struct_size < sizeof(*requested) ||
            requested->api_version != hal_v2_api_version ||
            !words_zero(requested->reserved.data(), requested->reserved.size())) {
            return HalV2Status::invalid_argument;
        }
        rtfw_device_init_config converted{};
        converted.struct_size = sizeof(converted);
        converted.abi_version = RTFW_DEVICE_ABI_VERSION;
        converted.requested_in_flight = requested->requested_in_flight;
        converted.requested_registered_buffers =
            requested->requested_registered_buffers;
        return static_cast<HalV2Status>(
            initialize_common(instance, &converted, kPathNativeV2));
    }

    static HalV2Status hal_register_buffer(
        void* instance,
        const HalV2BufferRegistration* registration,
        std::uint64_t* out_token) noexcept {
        if (!path_is(instance, kPathNativeV2) || !registration ||
            registration->struct_size < sizeof(*registration) ||
            !words_zero(
                registration->reserved.data(), registration->reserved.size())) {
            return HalV2Status::invalid_argument;
        }
        rtfw_device_buffer_registration converted{};
        converted.struct_size = sizeof(converted);
        converted.flags = registration->flags;
        converted.data = registration->data;
        converted.bytes = registration->bytes;
        std::copy(
            registration->name.begin(), registration->name.end(),
            std::begin(converted.name));
        return static_cast<HalV2Status>(
            register_buffer(instance, &converted, out_token));
    }

    static HalV2Status hal_unregister_buffer(
        void* instance,
        std::uint64_t token) noexcept {
        return path_is(instance, kPathNativeV2)
            ? static_cast<HalV2Status>(unregister_buffer(instance, token))
            : HalV2Status::invalid_state;
    }

    static HalV2Status hal_submit(
        void* instance,
        const HalV2Submission* submission) noexcept {
        if (!path_is(instance, kPathNativeV2) || !submission ||
            submission->struct_size < sizeof(*submission) ||
            submission->api_version != hal_v2_api_version ||
            !words_zero(
                submission->reserved.data(), submission->reserved.size())) {
            return HalV2Status::invalid_argument;
        }
        rtfw_device_submission converted{};
        converted.struct_size = sizeof(converted);
        converted.abi_version = RTFW_DEVICE_ABI_VERSION;
        converted.submission_id = submission->submission_id;
        converted.frame_index = submission->frame_index;
        converted.timeout_ns = submission->timeout_ns;
        converted.opcode = submission->opcode;
        converted.flags = submission->flags;
        converted.payload_size = submission->payload_size;
        converted.buffer_count = submission->buffer_count;
        std::copy(
            submission->payload.begin(), submission->payload.end(),
            std::begin(converted.payload));
        for (std::size_t index = 0; index < submission->buffers.size(); ++index) {
            converted.buffers[index].buffer_token =
                submission->buffers[index].buffer_token;
            converted.buffers[index].access = submission->buffers[index].access;
            converted.buffers[index].reserved0 =
                submission->buffers[index].reserved0;
            converted.buffers[index].offset = submission->buffers[index].offset;
            converted.buffers[index].bytes = submission->buffers[index].bytes;
        }
        return static_cast<HalV2Status>(submit(instance, &converted));
    }

    static HalV2Status hal_poll(
        void* instance,
        HalV2Completion* output,
        std::uint64_t capacity,
        std::uint64_t* out_count) noexcept {
        if (!path_is(instance, kPathNativeV2) ||
            capacity > kAbsoluteCapacityLimit ||
            (capacity != 0 && !output) || !out_count) {
            return HalV2Status::invalid_argument;
        }
        std::array<rtfw_device_completion, 16> scratch{};
        std::uint64_t total = 0;
        while (total < capacity) {
            const auto request = std::min<std::uint64_t>(
                capacity - total, scratch.size());
            std::uint64_t count = 0;
            const auto status = poll(instance, scratch.data(), request, &count);
            if (status != RTFW_DEVICE_STATUS_OK) {
                *out_count = total;
                return static_cast<HalV2Status>(status);
            }
            for (std::size_t index = 0; index < count; ++index) {
                output[total + index] = {};
                output[total + index].status = scratch[index].status;
                output[total + index].submission_id =
                    scratch[index].submission_id;
                output[total + index].device_timestamp_ns =
                    scratch[index].device_timestamp_ns;
                output[total + index].value = scratch[index].value;
            }
            total += count;
            if (count < request) {
                break;
            }
        }
        *out_count = total;
        return HalV2Status::ok;
    }

    static HalV2Status hal_cancel(
        void* instance,
        std::uint64_t submission_id) noexcept {
        return path_is(instance, kPathNativeV2)
            ? static_cast<HalV2Status>(cancel(instance, submission_id))
            : HalV2Status::invalid_state;
    }

    static HalV2Status hal_get_health(
        void* instance,
        HalV2Health* output) noexcept {
        if (!path_is(instance, kPathNativeV2) || !output ||
            output->struct_size < sizeof(*output)) {
            return HalV2Status::invalid_argument;
        }
        rtfw_device_health converted{};
        converted.struct_size = sizeof(converted);
        const auto status = get_health(instance, &converted);
        if (status != RTFW_DEVICE_STATUS_OK) {
            return static_cast<HalV2Status>(status);
        }
        *output = {};
        output->state = converted.state;
        output->last_status = converted.last_status;
        output->generation = converted.generation;
        output->submissions = converted.submissions;
        output->completions = converted.completions;
        output->queue_rejections = converted.queue_rejections;
        output->timeouts = converted.timeouts;
        output->errors = converted.errors;
        output->losses = converted.losses;
        output->cancellations = converted.cancellations;
        output->resets = converted.resets;
        output->outstanding = converted.outstanding;
        return HalV2Status::ok;
    }

    static HalV2Status hal_reset(void* instance) noexcept {
        return path_is(instance, kPathNativeV2)
            ? static_cast<HalV2Status>(reset(instance))
            : HalV2Status::invalid_state;
    }

    static HalV2Status hal_shutdown(void* instance) noexcept {
        return static_cast<HalV2Status>(
            shutdown_common(instance, kPathNativeV2));
    }

    static HalV2Status discover_memory(
        void* instance,
        HalV2MemoryTopologySnapshot* output) noexcept {
        auto* backend = self(instance);
        if (!backend || !output || output->struct_size < sizeof(*output) ||
            backend->driver.api_version != xdma_driver_api_version_2) {
            return HalV2Status::invalid_argument;
        }
        *output = {};
        output->memory_domain_count = 1;
        output->topology_node_count = 1;
        output->timestamp_domain_count = 1;
        output->completion_timestamp_domain_identity = 1;
        auto& domain = output->memory_domains[0];
        domain.identity = 1;
        domain.kind = static_cast<std::uint32_t>(HalV2MemoryDomainKind::host);
        domain.ownership_modes = hal_v2_memory_ownership_borrowed_host;
        domain.maximum_bytes = backend->config.max_buffer_bytes;
        domain.byte_granularity = backend->config.transfer_alignment;
        domain.alignment = backend->config.transfer_alignment;
        domain.offset_granularity = backend->config.transfer_alignment;
        domain.access = kKnownBufferFlags;
        domain.coherency = static_cast<std::uint32_t>(
            HalV2MemoryCoherency::host_coherent);
        domain.topology_node_identity = 1;
        domain.timestamp_domain_identity = 1;
        auto& node = output->topology_nodes[0];
        node.identity = 1;
        node.kind = static_cast<std::uint32_t>(
            HalV2TopologyNodeKind::dma_endpoint);
        auto& timestamp = output->timestamp_domains[0];
        timestamp.identity = 1;
        timestamp.kind = static_cast<std::uint32_t>(
            HalV2TimestampDomainKind::runtime_monotonic);
        timestamp.tick_numerator_ns = 1;
        timestamp.tick_denominator = 1;
        timestamp.monotonic = 1;
        return HalV2Status::ok;
    }

    static HalV2Status register_memory(
        void* instance,
        const HalV2MemoryRegistration* registration,
        HalV2MemoryToken* token) noexcept {
        if (token) {
            *token = {};
        }
        if (!registration || !token ||
            registration->struct_size != sizeof(*registration) ||
            registration->extension_version !=
                hal_v2_memory_topology_extension_version ||
            registration->domain_identity != 1 ||
            registration->ownership != static_cast<std::uint32_t>(
                HalV2MemoryOwnership::borrowed_host) ||
            registration->coherency != static_cast<std::uint32_t>(
                HalV2MemoryCoherency::host_coherent) ||
            registration->synchronization != hal_v2_memory_sync_none ||
            !registration->host_data ||
            registration->opaque_handle.size != 0 ||
            !words_zero(
                registration->reserved.data(), registration->reserved.size())) {
            return HalV2Status::invalid_argument;
        }
        HalV2BufferRegistration legacy{};
        legacy.flags = registration->access;
        legacy.data = registration->host_data;
        legacy.bytes = registration->bytes;
        legacy.name = registration->name;
        std::uint64_t submission_token = 0;
        const auto status = hal_register_buffer(
            instance, &legacy, &submission_token);
        if (status == HalV2Status::ok) {
            token->submission_token = submission_token;
        }
        return status;
    }

    static HalV2Status unregister_memory(
        void* instance,
        const HalV2MemoryRegistration* registration,
        const HalV2MemoryToken* token) noexcept {
        if (!registration || !token || token->struct_size != sizeof(*token) ||
            token->extension_version !=
                hal_v2_memory_topology_extension_version ||
            token->submission_token == 0 || token->native_token.size != 0 ||
            !words_zero(token->reserved.data(), token->reserved.size())) {
            return HalV2Status::invalid_argument;
        }
        return hal_unregister_buffer(instance, token->submission_token);
    }

    static HalV2Status query_correlation(
        void*,
        const HalV2TimestampCorrelationQuery*,
        HalV2TimestampCorrelation*) noexcept {
        return HalV2Status::unsupported;
    }

    static HalV2Status get_command_capabilities(
        void* instance,
        HalV2CommandTimelineCapabilities* output) noexcept {
        auto* backend = self(instance);
        if (!backend || !output || output->struct_size < sizeof(*output) ||
            backend->driver.api_version != xdma_driver_api_version_2) {
            return HalV2Status::invalid_argument;
        }
        *output = {};
        output->max_in_flight_batches =
            static_cast<std::uint32_t>(std::min<std::size_t>(
                backend->config.queue_capacity,
                std::numeric_limits<std::uint32_t>::max()));
        output->max_commands_per_batch = hal_v2_command_capacity;
        output->max_wait_points = hal_v2_timeline_wait_capacity;
        output->max_signal_points = hal_v2_timeline_signal_capacity;
        output->max_timelines = hal_v2_timeline_capacity;
        output->completion_batch_capacity = output->max_in_flight_batches;
        output->backend_control_storage_bytes =
            backend->config.queue_capacity * sizeof(BatchSlot);
        return HalV2Status::ok;
    }

    static HalV2Status submit_batch(
        void* instance,
        const DeviceCommandBatch* batch) noexcept {
        auto* backend = self(instance);
        if (!backend || !batch) {
            return HalV2Status::invalid_argument;
        }
        if (!backend->initialized.load(std::memory_order_acquire) ||
            backend->registration_path.load(std::memory_order_acquire) !=
                kPathNativeV2 ||
            !backend->accepting.load(std::memory_order_acquire)) {
            return HalV2Status::invalid_state;
        }
        if (!backend->batch_valid(*batch)) {
            return HalV2Status::invalid_argument;
        }
        BatchSlot* slot = nullptr;
        for (std::size_t index = 0;
             index < backend->active_queue_capacity; ++index) {
            auto expected = kSlotFree;
            if (backend->batch_slots[index].state.compare_exchange_strong(
                    expected, kSlotOwned, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                slot = &backend->batch_slots[index];
                break;
            }
        }
        if (!slot) {
            backend->queue_rejections.fetch_add(
                1, std::memory_order_relaxed);
            return HalV2Status::queue_full;
        }
        slot->batch = *batch;
        slot->sequence = backend->batch_sequence.fetch_add(
            1, std::memory_order_relaxed);
        slot->started_ns = backend->driver.monotonic_time_ns(
            backend->driver.user_data);
        if (slot->started_ns >
            std::numeric_limits<std::uint64_t>::max() - batch->timeout_ns) {
            slot->state.store(kSlotFree, std::memory_order_release);
            return HalV2Status::invalid_argument;
        }
        slot->canceled.store(false, std::memory_order_relaxed);
        slot->active_event_wait.store(false, std::memory_order_relaxed);
        slot->completion = {};
        backend->submissions.fetch_add(1, std::memory_order_relaxed);
        backend->outstanding.fetch_add(1, std::memory_order_relaxed);
        slot->state.store(kSlotQueued, std::memory_order_release);
        backend->work_epoch.fetch_add(1, std::memory_order_release);
        backend->work_epoch.notify_all();
        return HalV2Status::ok;
    }

    static HalV2Status poll_batches(
        void* instance,
        HalV2BatchCompletion* output,
        std::uint64_t capacity,
        std::uint64_t* out_count) noexcept {
        auto* backend = self(instance);
        if (!backend || !out_count || (capacity != 0 && !output) ||
            !backend->initialized.load(std::memory_order_acquire)) {
            return HalV2Status::invalid_argument;
        }
        *out_count = 0;
        for (std::size_t index = 0;
             index < backend->active_queue_capacity && *out_count < capacity;
             ++index) {
            auto& slot = backend->batch_slots[index];
            auto expected = kSlotComplete;
            if (!slot.state.compare_exchange_strong(
                    expected, kSlotReaping, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                continue;
            }
            output[*out_count] = slot.completion;
            ++*out_count;
            backend->account_completion(static_cast<rtfw_device_status>(
                slot.completion.status));
            backend->completions.fetch_add(1, std::memory_order_relaxed);
            backend->outstanding.fetch_sub(1, std::memory_order_relaxed);
            slot.state.store(kSlotFree, std::memory_order_release);
        }
        return HalV2Status::ok;
    }

    static HalV2Status cancel_batch(
        void* instance,
        std::uint64_t batch_id) noexcept {
        auto* backend = self(instance);
        if (!backend || batch_id == 0 ||
            !backend->initialized.load(std::memory_order_acquire)) {
            return HalV2Status::invalid_argument;
        }
        for (std::size_t index = 0;
             index < backend->active_queue_capacity; ++index) {
            auto& slot = backend->batch_slots[index];
            auto state = slot.state.load(std::memory_order_acquire);
            if (state == kSlotFree || slot.batch.batch_id != batch_id) {
                continue;
            }
            if (state == kSlotQueued && slot.state.compare_exchange_strong(
                    state, kSlotOwned, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                slot.canceled.store(true, std::memory_order_release);
                slot.completion = {};
                slot.completion.status = static_cast<std::int32_t>(
                    HalV2Status::canceled);
                slot.completion.batch_id = slot.batch.batch_id;
                slot.completion.device_timestamp =
                    backend->driver.monotonic_time_ns(
                        backend->driver.user_data);
                slot.completion.timestamp_domain_identity = 1;
                slot.completion.signal_count = slot.batch.signal_count;
                std::copy_n(slot.batch.signals.begin(),
                            slot.batch.signal_count,
                            slot.completion.signals.begin());
                slot.state.store(kSlotComplete, std::memory_order_release);
                backend->completion_epoch.fetch_add(
                    1, std::memory_order_release);
                backend->completion_epoch.notify_all();
                return HalV2Status::ok;
            }
            if (state == kSlotRunning) {
                slot.canceled.store(true, std::memory_order_release);
                if (slot.state.load(std::memory_order_acquire) ==
                        kSlotRunning &&
                    slot.active_event_wait.load(std::memory_order_acquire) &&
                    backend->driver.request_stop) {
                    const auto result = backend->driver.request_stop(
                        backend->driver.user_data);
                    if (result != XdmaDriverResult::success) {
                        return normalize_hal(result);
                    }
                }
                return HalV2Status::ok;
            }
            return HalV2Status::invalid_argument;
        }
        return HalV2Status::invalid_argument;
    }

    static HalV2Status request_command_stop(void* instance) noexcept {
        auto* backend = self(instance);
        if (!backend) {
            return HalV2Status::invalid_argument;
        }
        backend->accepting.store(false, std::memory_order_release);
        backend->stop_requested.store(true, std::memory_order_release);
        bool active_event_wait = false;
        for (std::size_t index = 0;
             index < backend->active_queue_capacity; ++index) {
            const auto state = backend->batch_slots[index].state.load(
                std::memory_order_acquire);
            if (state == kSlotQueued || state == kSlotRunning) {
                backend->batch_slots[index].canceled.store(
                    true, std::memory_order_release);
            }
            active_event_wait = active_event_wait ||
                backend->batch_slots[index].active_event_wait.load(
                    std::memory_order_acquire);
        }
        if (active_event_wait && backend->driver.request_stop) {
            const auto status = backend->driver.request_stop(
                backend->driver.user_data);
            if (status != XdmaDriverResult::success) {
                return normalize_hal(status);
            }
        }
        backend->work_epoch.fetch_add(1, std::memory_order_release);
        backend->work_epoch.notify_all();
        return HalV2Status::ok;
    }

    XdmaDriverApi driver;
    XdmaBackendConfig config;
    std::unique_ptr<Slot[]> slots;
    std::unique_ptr<BatchSlot[]> batch_slots;
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
    std::atomic<std::uint32_t> registration_path{kPathNone};
    std::atomic<std::size_t> slot_hint{0};
    std::atomic<std::uint64_t> batch_sequence{0};
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
    std::atomic<std::uint64_t> cancellations{0};
    std::atomic<std::uint64_t> resets{0};
    std::atomic<std::uint64_t> outstanding{0};
    HalV2BackendApi hal_api{};
    HalV2MemoryTopologyExtension memory_extension{};
    HalV2CommandTimelineExtension command_extension{};
};

void XdmaDeviceBackend::Impl::initialize_hal_tables() noexcept {
    hal_api = {};
    hal_api.instance = this;
    hal_api.get_capabilities = &hal_get_capabilities;
    hal_api.initialize = &hal_initialize;
    hal_api.register_buffer = &hal_register_buffer;
    hal_api.unregister_buffer = &hal_unregister_buffer;
    hal_api.submit = &hal_submit;
    hal_api.poll = &hal_poll;
    hal_api.cancel = &hal_cancel;
    hal_api.get_health = &hal_get_health;
    hal_api.reset = &hal_reset;
    hal_api.shutdown = &hal_shutdown;

    memory_extension = {};
    memory_extension.instance = this;
    memory_extension.discover = &discover_memory;
    memory_extension.register_memory = &register_memory;
    memory_extension.unregister_memory = &unregister_memory;
    memory_extension.query_timestamp_correlation = &query_correlation;

    command_extension = {};
    command_extension.instance = this;
    command_extension.get_capabilities = &get_command_capabilities;
    command_extension.submit = &submit_batch;
    command_extension.poll = &poll_batches;
    command_extension.cancel = &cancel_batch;
    command_extension.request_stop = &request_command_stop;
}

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
    table.initialize = &Impl::initialize_v1;
    table.register_buffer = &Impl::register_buffer_v1;
    table.unregister_buffer = &Impl::unregister_buffer_v1;
    table.submit = &Impl::submit_v1;
    table.poll = &Impl::poll_v1;
    table.cancel = &Impl::cancel_v1;
    table.get_health = &Impl::get_health_v1;
    table.reset = &Impl::reset_v1;
    table.shutdown = &Impl::shutdown_v1;
    return table;
}

HalV2BackendRegistration XdmaDeviceBackend::hal_v2_registration(
    std::string_view name) noexcept {
    HalV2BackendRegistration registration{};
    registration.name = name;
    if (!impl_ ||
        impl_->driver.api_version != xdma_driver_api_version_2) {
        return registration;
    }
    registration.api = impl_->hal_api;
    registration.memory_topology = &impl_->memory_extension;
    registration.command_timeline = &impl_->command_extension;
    return registration;
}

} // namespace rt

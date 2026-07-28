#include <rt/cuda_backend.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t kSlotFree = 0;
constexpr std::uint32_t kSlotOwned = 1;
constexpr std::uint32_t kSlotPending = 2;
constexpr std::uint32_t kSlotQuarantined = 3;
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

bool valid_identifier(
    std::string_view value,
    std::size_t capacity) noexcept {
    if (value.empty() || value.size() >= capacity) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        identifier_character);
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

bool valid_device_range(
    rt::CudaDeviceAddress address,
    std::uint64_t bytes) noexcept {
    return address != 0 &&
           bytes != 0 &&
           address <=
               std::numeric_limits<rt::CudaDeviceAddress>::max() -
                   (bytes - 1);
}

bool driver_api_valid(const rt::CudaDriverApi& driver) noexcept {
    return driver.struct_size >= sizeof(driver) &&
           driver.api_version == rt::cuda_driver_api_version &&
           driver.push_context &&
           driver.pop_context &&
           driver.event_create &&
           driver.event_destroy &&
           driver.event_record &&
           driver.event_query &&
           driver.event_synchronize &&
           driver.stream_synchronize &&
           driver.mem_alloc &&
           driver.mem_free &&
           driver.host_register &&
           driver.host_unregister &&
           driver.memcpy_host_to_device_async &&
           driver.memcpy_device_to_host_async &&
           driver.memcpy_device_to_device_async &&
           driver.memset_d8_async &&
           driver.launch_kernel &&
           driver.monotonic_time_ns &&
           bytes_zero(driver.reserved, std::size(driver.reserved));
}

rt::CudaBackendConfig validated_config(
    const rt::CudaDriverApi& driver,
    const rt::CudaBackendConfig& config) {
    if (!driver_api_valid(driver) ||
        config.queue_capacity == 0 ||
        config.queue_capacity > kAbsoluteCapacityLimit ||
        config.buffer_capacity == 0 ||
        config.buffer_capacity > kAbsoluteCapacityLimit ||
        config.kernel_capacity == 0 ||
        config.kernel_capacity > kAbsoluteCapacityLimit ||
        config.context == 0 ||
        config.streams.empty() ||
        config.streams.size() > config.queue_capacity) {
        throw std::invalid_argument(
            "invalid CUDA backend driver, context, stream, or capacity");
    }
    for (const auto stream : config.streams) {
        if (stream == 0) {
            throw std::invalid_argument(
                "CUDA backend stream handles must be nonzero");
        }
    }
    return config;
}

rtfw_device_status device_status(
    rt::CudaDriverResult result) noexcept {
    switch (result) {
    case rt::CudaDriverResult::success:
        return RTFW_DEVICE_STATUS_OK;
    case rt::CudaDriverResult::not_ready:
        return RTFW_DEVICE_STATUS_INVALID_STATE;
    case rt::CudaDriverResult::invalid_value:
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    case rt::CudaDriverResult::out_of_memory:
        return RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED;
    case rt::CudaDriverResult::context_lost:
        return RTFW_DEVICE_STATUS_LOST;
    case rt::CudaDriverResult::launch_failure:
        return RTFW_DEVICE_STATUS_RESET_REQUIRED;
    case rt::CudaDriverResult::error:
        return RTFW_DEVICE_STATUS_ERROR;
    }
    return RTFW_DEVICE_STATUS_INTERNAL_ERROR;
}

rt::CudaDriverResult combine_with_pop(
    rt::CudaDriverResult primary,
    rt::CudaDriverResult pop) noexcept {
    if (pop == rt::CudaDriverResult::context_lost) {
        return pop;
    }
    return primary == rt::CudaDriverResult::success
        ? pop
        : primary;
}

} // namespace

namespace rt {

struct CudaDeviceBackend::Impl {
    struct Slot {
        std::atomic<std::uint32_t> state{kSlotFree};
        CudaEvent event = 0;
        CudaStream stream = 0;
        std::uint64_t submission_id = 0;
        std::uint64_t started_ns = 0;
        std::uint64_t timeout_ns = 0;
        std::uint32_t buffer_count = 0;
        std::array<
            std::uint64_t,
            RTFW_DEVICE_BUFFER_REF_CAPACITY> buffer_tokens{};
        bool timed_out = false;
    };

    struct Buffer {
        void* host_data = nullptr;
        CudaDeviceAddress device_address = 0;
        std::uint64_t bytes = 0;
        std::uint32_t flags = 0;
        std::uint64_t token = 0;
        bool registered = false;
        bool owns_device_address = false;
        bool owns_host_registration = false;
        std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
    };

    struct Binding {
        CudaDeviceAddress device_address = 0;
        std::uint64_t bytes = 0;
        bool bound = false;
        std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
    };

    struct Kernel {
        CudaFunction function = 0;
        std::uint64_t token = 0;
        bool registered = false;
    };

    Impl(
        const CudaDriverApi& requested_driver,
        const CudaBackendConfig& requested_config)
        : driver(requested_driver),
          config(validated_config(
              requested_driver,
              requested_config)),
          streams(
              std::make_unique<CudaStream[]>(
                  config.streams.size())),
          stream_count(config.streams.size()),
          slots(
              std::make_unique<Slot[]>(
                  config.queue_capacity)),
          buffers(
              std::make_unique<Buffer[]>(
                  config.buffer_capacity)),
          bindings(
              std::make_unique<Binding[]>(
                  config.buffer_capacity)),
          kernels(
              std::make_unique<Kernel[]>(
                  config.kernel_capacity)) {
        std::copy(
            config.streams.begin(),
            config.streams.end(),
            streams.get());
        for (std::size_t index = 0;
             index < config.queue_capacity;
             ++index) {
            slots[index].stream = streams[index % stream_count];
        }
        config.streams = {};
    }

    [[nodiscard]] static Impl* self(void* instance) noexcept {
        return static_cast<Impl*>(instance);
    }

    [[nodiscard]] Buffer* buffer_for(std::uint64_t token) noexcept {
        if (token == 0 ||
            token > config.buffer_capacity) {
            return nullptr;
        }
        auto& buffer = buffers[
            static_cast<std::size_t>(token - 1)];
        return buffer.registered && buffer.token == token
            ? &buffer
            : nullptr;
    }

    [[nodiscard]] const Buffer* buffer_for(
        std::uint64_t token) const noexcept {
        return const_cast<Impl*>(this)->buffer_for(token);
    }

    [[nodiscard]] Kernel* kernel_for(std::uint64_t token) noexcept {
        if (token == 0 ||
            token > config.kernel_capacity) {
            return nullptr;
        }
        auto& kernel = kernels[
            static_cast<std::size_t>(token - 1)];
        return kernel.registered && kernel.token == token
            ? &kernel
            : nullptr;
    }

    [[nodiscard]] const Kernel* kernel_for(
        std::uint64_t token) const noexcept {
        return const_cast<Impl*>(this)->kernel_for(token);
    }

    [[nodiscard]] CudaDriverResult push_context() noexcept {
        return driver.push_context(
            driver.user_data,
            config.context);
    }

    [[nodiscard]] CudaDriverResult pop_context() noexcept {
        CudaContext popped = 0;
        const auto result =
            driver.pop_context(driver.user_data, &popped);
        if (result != CudaDriverResult::success) {
            return result;
        }
        return popped == config.context
            ? CudaDriverResult::success
            : CudaDriverResult::context_lost;
    }

    void escalate_health(
        rtfw_device_health_state requested) noexcept {
        auto current = health_state.load(std::memory_order_acquire);
        while (current < requested &&
               !health_state.compare_exchange_weak(
                   current,
                   requested,
                   std::memory_order_release,
                   std::memory_order_acquire)) {
        }
    }

    void set_health_after(rtfw_device_status status) noexcept {
        last_status.store(status, std::memory_order_release);
        switch (status) {
        case RTFW_DEVICE_STATUS_OK:
        case RTFW_DEVICE_STATUS_CANCELED:
            break;
        case RTFW_DEVICE_STATUS_TIMEOUT:
            timeouts.fetch_add(1, std::memory_order_relaxed);
            escalate_health(RTFW_DEVICE_HEALTH_DEGRADED);
            break;
        case RTFW_DEVICE_STATUS_LOST:
            losses.fetch_add(1, std::memory_order_relaxed);
            permanent_loss.store(true, std::memory_order_release);
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

    [[nodiscard]] bool reference_valid(
        const rtfw_device_buffer_ref& reference) const noexcept {
        const auto* buffer = buffer_for(reference.buffer_token);
        return buffer &&
               reference.reserved0 == 0 &&
               valid_access(reference.access) &&
               reference.offset <= buffer->bytes &&
               reference.bytes <= buffer->bytes - reference.offset;
    }

    [[nodiscard]] bool has_access(
        const rtfw_device_buffer_ref& reference,
        std::uint32_t access) const noexcept {
        return (reference.access & access) == access;
    }

    [[nodiscard]] bool has_flags(
        const rtfw_device_buffer_ref& reference,
        std::uint32_t flags) const noexcept {
        const auto* buffer = buffer_for(reference.buffer_token);
        return buffer && (buffer->flags & flags) == flags;
    }

    [[nodiscard]] bool kernel_launch_valid(
        const rtfw_device_submission& submission,
        const CudaKernelLaunch& launch) const noexcept {
        if (submission.payload_size != sizeof(launch) ||
            !kernel_for(launch.kernel_token) ||
            launch.grid_x == 0 ||
            launch.grid_y == 0 ||
            launch.grid_z == 0 ||
            launch.block_x == 0 ||
            launch.block_y == 0 ||
            launch.block_z == 0 ||
            launch.argument_count >
                cuda_kernel_argument_capacity ||
            launch.scalar_data_size >
                cuda_kernel_scalar_capacity ||
            launch.reserved0[0] != 0 ||
            launch.reserved0[1] != 0 ||
            !bytes_zero(
                launch.reserved,
                std::size(launch.reserved))) {
            return false;
        }
        for (std::size_t index = 0;
             index < launch.argument_count;
             ++index) {
            const auto& argument = launch.arguments[index];
            const auto kind =
                static_cast<CudaKernelArgumentKind>(argument.kind);
            if (kind == CudaKernelArgumentKind::buffer_address) {
                if (argument.buffer_index >= submission.buffer_count ||
                    argument.scalar_offset != 0 ||
                    argument.scalar_size != 0) {
                    return false;
                }
                const auto& reference =
                    submission.buffers[argument.buffer_index];
                if (reference.bytes == 0) {
                    return false;
                }
                const auto* buffer =
                    buffer_for(reference.buffer_token);
                const bool reads = has_access(
                    reference,
                    RTFW_DEVICE_ACCESS_READ);
                const bool writes = has_access(
                    reference,
                    RTFW_DEVICE_ACCESS_WRITE);
                if (!buffer ||
                    (reads &&
                     (buffer->flags &
                      RTFW_DEVICE_BUFFER_DEVICE_READ) == 0) ||
                    (writes &&
                     (buffer->flags &
                      RTFW_DEVICE_BUFFER_DEVICE_WRITE) == 0)) {
                    return false;
                }
            } else if (kind == CudaKernelArgumentKind::scalar) {
                if (argument.buffer_index != 0 ||
                    argument.scalar_size == 0 ||
                    argument.scalar_size > sizeof(std::uint64_t) ||
                    argument.scalar_offset >
                        launch.scalar_data_size ||
                    argument.scalar_size >
                        launch.scalar_data_size -
                            argument.scalar_offset) {
                    return false;
                }
            } else {
                return false;
            }
        }
        for (std::size_t index = launch.argument_count;
             index < launch.arguments.size();
             ++index) {
            const auto& argument = launch.arguments[index];
            if (argument.kind != 0 ||
                argument.buffer_index != 0 ||
                argument.scalar_offset != 0 ||
                argument.scalar_size != 0) {
                return false;
            }
        }
        for (std::size_t index = launch.scalar_data_size;
             index < launch.scalar_data.size();
             ++index) {
            if (launch.scalar_data[index] != std::byte{0}) {
                return false;
            }
        }
        return true;
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
            if (!reference_valid(submission.buffers[index])) {
                return false;
            }
        }

        switch (submission.opcode) {
        case cuda_device_opcode_noop:
            return submission.payload_size == 0 &&
                   submission.buffer_count == 0;
        case cuda_device_opcode_copy_host_to_device:
            return submission.payload_size == 0 &&
                   submission.buffer_count == 1 &&
                   submission.buffers[0].bytes != 0 &&
                   has_access(
                       submission.buffers[0],
                       RTFW_DEVICE_ACCESS_READ) &&
                   has_flags(
                       submission.buffers[0],
                       RTFW_DEVICE_BUFFER_HOST_READ |
                           RTFW_DEVICE_BUFFER_DEVICE_WRITE);
        case cuda_device_opcode_copy_device_to_host:
            return submission.payload_size == 0 &&
                   submission.buffer_count == 1 &&
                   submission.buffers[0].bytes != 0 &&
                   has_access(
                       submission.buffers[0],
                       RTFW_DEVICE_ACCESS_WRITE) &&
                   has_flags(
                       submission.buffers[0],
                       RTFW_DEVICE_BUFFER_HOST_WRITE |
                           RTFW_DEVICE_BUFFER_DEVICE_READ);
        case cuda_device_opcode_copy_device_to_device:
            return submission.payload_size == 0 &&
                   submission.buffer_count == 2 &&
                   submission.buffers[0].bytes != 0 &&
                   submission.buffers[0].bytes ==
                       submission.buffers[1].bytes &&
                   (submission.buffers[0].buffer_token !=
                        submission.buffers[1].buffer_token ||
                    submission.buffers[0].offset +
                            submission.buffers[0].bytes <=
                        submission.buffers[1].offset ||
                    submission.buffers[1].offset +
                            submission.buffers[1].bytes <=
                        submission.buffers[0].offset) &&
                   has_access(
                       submission.buffers[0],
                       RTFW_DEVICE_ACCESS_READ) &&
                   has_access(
                       submission.buffers[1],
                       RTFW_DEVICE_ACCESS_WRITE) &&
                   has_flags(
                       submission.buffers[0],
                       RTFW_DEVICE_BUFFER_DEVICE_READ) &&
                   has_flags(
                       submission.buffers[1],
                       RTFW_DEVICE_BUFFER_DEVICE_WRITE);
        case cuda_device_opcode_memset_d8:
            return submission.payload_size == 1 &&
                   submission.buffer_count == 1 &&
                   submission.buffers[0].bytes != 0 &&
                   has_access(
                       submission.buffers[0],
                       RTFW_DEVICE_ACCESS_WRITE) &&
                   has_flags(
                       submission.buffers[0],
                       RTFW_DEVICE_BUFFER_DEVICE_WRITE);
        case cuda_device_opcode_launch_kernel: {
            CudaKernelLaunch launch{};
            if (submission.payload_size != sizeof(launch)) {
                return false;
            }
            std::memcpy(
                &launch,
                submission.payload,
                sizeof(launch));
            return kernel_launch_valid(submission, launch);
        }
        default:
            return false;
        }
    }

    [[nodiscard]] CudaDriverResult execute(
        const rtfw_device_submission& submission,
        CudaStream stream) noexcept {
        if (submission.opcode == cuda_device_opcode_noop) {
            return CudaDriverResult::success;
        }
        if (submission.opcode ==
            cuda_device_opcode_copy_host_to_device) {
            const auto& reference = submission.buffers[0];
            const auto* buffer = buffer_for(reference.buffer_token);
            return driver.memcpy_host_to_device_async(
                driver.user_data,
                buffer->device_address + reference.offset,
                static_cast<const std::byte*>(buffer->host_data) +
                    reference.offset,
                reference.bytes,
                stream);
        }
        if (submission.opcode ==
            cuda_device_opcode_copy_device_to_host) {
            const auto& reference = submission.buffers[0];
            const auto* buffer = buffer_for(reference.buffer_token);
            return driver.memcpy_device_to_host_async(
                driver.user_data,
                static_cast<std::byte*>(buffer->host_data) +
                    reference.offset,
                buffer->device_address + reference.offset,
                reference.bytes,
                stream);
        }
        if (submission.opcode ==
            cuda_device_opcode_copy_device_to_device) {
            const auto& source = submission.buffers[0];
            const auto& destination = submission.buffers[1];
            const auto* source_buffer =
                buffer_for(source.buffer_token);
            const auto* destination_buffer =
                buffer_for(destination.buffer_token);
            return driver.memcpy_device_to_device_async(
                driver.user_data,
                destination_buffer->device_address +
                    destination.offset,
                source_buffer->device_address + source.offset,
                source.bytes,
                stream);
        }
        if (submission.opcode == cuda_device_opcode_memset_d8) {
            const auto& reference = submission.buffers[0];
            const auto* buffer = buffer_for(reference.buffer_token);
            return driver.memset_d8_async(
                driver.user_data,
                buffer->device_address + reference.offset,
                submission.payload[0],
                reference.bytes,
                stream);
        }

        CudaKernelLaunch launch{};
        std::memcpy(
            &launch,
            submission.payload,
            sizeof(launch));
        const auto* kernel = kernel_for(launch.kernel_token);
        std::array<
            std::uint64_t,
            cuda_kernel_argument_capacity> values{};
        std::array<
            void*,
            cuda_kernel_argument_capacity> arguments{};
        for (std::size_t index = 0;
             index < launch.argument_count;
             ++index) {
            const auto& argument = launch.arguments[index];
            const auto kind =
                static_cast<CudaKernelArgumentKind>(argument.kind);
            if (kind == CudaKernelArgumentKind::buffer_address) {
                const auto& reference =
                    submission.buffers[argument.buffer_index];
                const auto* buffer =
                    buffer_for(reference.buffer_token);
                values[index] =
                    buffer->device_address + reference.offset;
            } else {
                std::memcpy(
                    &values[index],
                    launch.scalar_data.data() +
                        argument.scalar_offset,
                    argument.scalar_size);
            }
            arguments[index] = &values[index];
        }
        return driver.launch_kernel(
            driver.user_data,
            kernel->function,
            launch.grid_x,
            launch.grid_y,
            launch.grid_z,
            launch.block_x,
            launch.block_y,
            launch.block_z,
            launch.dynamic_shared_bytes,
            stream,
            launch.argument_count == 0
                ? nullptr
                : arguments.data());
    }

    [[nodiscard]] Slot* acquire_slot() noexcept {
        const auto first =
            slot_hint.fetch_add(
                1,
                std::memory_order_relaxed) %
            config.queue_capacity;
        for (std::size_t offset = 0;
             offset < config.queue_capacity;
             ++offset) {
            auto& slot = slots[
                (first + offset) % config.queue_capacity];
            auto expected = kSlotFree;
            if (slot.state.compare_exchange_strong(
                    expected,
                    kSlotOwned,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return &slot;
            }
        }
        return nullptr;
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
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max());
        capabilities->inline_payload_capacity =
            RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY;
        capabilities->buffer_ref_capacity =
            RTFW_DEVICE_BUFFER_REF_CAPACITY;
        capabilities->supports_cancel = 0;
        capabilities->supports_reset = 1;
        capabilities->deterministic_mock = 0;
        std::memcpy(
            capabilities->backend_id,
            "rtfw.cuda.driver.v1",
            sizeof("rtfw.cuda.driver.v1"));
        capabilities->control_storage_bytes =
            sizeof(Impl) +
            (backend->config.queue_capacity * sizeof(Slot)) +
            (backend->config.buffer_capacity *
             (sizeof(Buffer) + sizeof(Binding))) +
            (backend->config.kernel_capacity * sizeof(Kernel)) +
            (backend->stream_count * sizeof(CudaStream));
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
        if (backend->shutdown_incomplete.load(
                std::memory_order_acquire)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        if (backend->permanent_loss.load(std::memory_order_acquire)) {
            return RTFW_DEVICE_STATUS_LOST;
        }
        bool expected = false;
        if (!backend->initialized.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }

        auto result = backend->push_context();
        if (result != CudaDriverResult::success) {
            backend->initialized.store(false, std::memory_order_release);
            const auto status = device_status(result);
            backend->set_health_after(status);
            return status;
        }
        std::size_t created = 0;
        for (; created < backend->config.queue_capacity; ++created) {
            auto& slot = backend->slots[created];
            slot.state.store(kSlotFree, std::memory_order_relaxed);
            CudaEvent event = 0;
            result = backend->driver.event_create(
                backend->driver.user_data,
                &event);
            if (result != CudaDriverResult::success ||
                event == 0) {
                if (event != 0) {
                    slot.event = event;
                }
                break;
            }
            slot.event = event;
        }
        if (created != backend->config.queue_capacity) {
            bool cleanup_incomplete = false;
            for (std::size_t index = created + 1;
                 index != 0;
                 --index) {
                auto& slot = backend->slots[index - 1];
                if (slot.event == 0) {
                    continue;
                }
                const auto cleanup_result =
                    backend->driver.event_destroy(
                        backend->driver.user_data,
                        slot.event);
                if (cleanup_result == CudaDriverResult::success) {
                    slot.event = 0;
                } else {
                    cleanup_incomplete = true;
                }
            }
            const auto pop_result = backend->pop_context();
            cleanup_incomplete =
                cleanup_incomplete ||
                pop_result != CudaDriverResult::success;
            result = combine_with_pop(result, pop_result);
            backend->shutdown_incomplete.store(
                cleanup_incomplete,
                std::memory_order_release);
            backend->initialized.store(false, std::memory_order_release);
            const auto status =
                result == CudaDriverResult::success
                ? RTFW_DEVICE_STATUS_INTERNAL_ERROR
                : device_status(result);
            backend->set_health_after(status);
            return status;
        }
        result = backend->pop_context();
        if (result != CudaDriverResult::success) {
            bool cleanup_incomplete = true;
            // A failed pop leaves context state uncertain. Preserve any event
            // whose destruction also fails so shutdown can retry only the
            // unresolved handles before initialization is attempted again.
            for (std::size_t index = 0;
                 index < backend->config.queue_capacity;
                 ++index) {
                auto& slot = backend->slots[index];
                if (slot.event != 0) {
                    const auto cleanup_result =
                        backend->driver.event_destroy(
                            backend->driver.user_data,
                            slot.event);
                    if (cleanup_result ==
                        CudaDriverResult::success) {
                        slot.event = 0;
                    } else {
                        cleanup_incomplete = true;
                    }
                }
            }
            backend->shutdown_incomplete.store(
                cleanup_incomplete,
                std::memory_order_release);
            backend->initialized.store(false, std::memory_order_release);
            const auto status = device_status(result);
            backend->set_health_after(status);
            return status;
        }
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
        constexpr auto valid_flags =
            RTFW_DEVICE_BUFFER_HOST_READ |
            RTFW_DEVICE_BUFFER_HOST_WRITE |
            RTFW_DEVICE_BUFFER_DEVICE_READ |
            RTFW_DEVICE_BUFFER_DEVICE_WRITE;
        if (!backend || !registration || !out_token ||
            !backend->initialized.load(std::memory_order_acquire) ||
            registration->struct_size < sizeof(*registration) ||
            !registration->data ||
            registration->bytes == 0 ||
            registration->bytes >
                std::numeric_limits<std::size_t>::max() ||
            registration->flags == 0 ||
            (registration->flags & ~valid_flags) != 0 ||
            !valid_identifier(
                registration->name,
                RTFW_DEVICE_IDENTIFIER_CAPACITY) ||
            !bytes_zero(
                registration->reserved,
                std::size(registration->reserved))) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        *out_token = 0;
        const auto health =
            backend->health_state.load(std::memory_order_acquire);
        if (health == RTFW_DEVICE_HEALTH_LOST) {
            return RTFW_DEVICE_STATUS_LOST;
        }
        if (health == RTFW_DEVICE_HEALTH_RESET_REQUIRED) {
            return RTFW_DEVICE_STATUS_RESET_REQUIRED;
        }
        for (std::size_t index = 0;
             index < backend->config.buffer_capacity;
             ++index) {
            const auto& buffer = backend->buffers[index];
            if (buffer.registered &&
                std::strncmp(
                    buffer.name.data(),
                    registration->name,
                    RTFW_DEVICE_IDENTIFIER_CAPACITY) == 0) {
                return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
            }
        }

        std::size_t free_index = backend->config.buffer_capacity;
        for (std::size_t index = 0;
             index < backend->config.buffer_capacity;
             ++index) {
            if (!backend->buffers[index].registered) {
                free_index = index;
                break;
            }
        }
        if (free_index == backend->config.buffer_capacity) {
            return RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED;
        }

        CudaDeviceAddress address = 0;
        bool owned = false;
        for (std::size_t index = 0;
             index < backend->config.buffer_capacity;
             ++index) {
            const auto& binding = backend->bindings[index];
            if (binding.bound &&
                std::strncmp(
                    binding.name.data(),
                    registration->name,
                    RTFW_DEVICE_IDENTIFIER_CAPACITY) == 0) {
                if (registration->bytes > binding.bytes) {
                    return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
                }
                address = binding.device_address;
                break;
            }
        }
        if (address == 0 &&
            !backend->config.allocate_device_mirrors) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        const bool allocate_device_address = address == 0;
        bool host_registered = false;
        if (allocate_device_address ||
            backend->config.register_host_memory) {
            auto result = backend->push_context();
            const bool context_pushed =
                result == CudaDriverResult::success;
            if (result == CudaDriverResult::success &&
                allocate_device_address) {
                result = backend->driver.mem_alloc(
                    backend->driver.user_data,
                    registration->bytes,
                    &address);
                if (result == CudaDriverResult::success &&
                    !valid_device_range(
                        address,
                        registration->bytes)) {
                    result = CudaDriverResult::invalid_value;
                }
            }
            if (result == CudaDriverResult::success &&
                backend->config.register_host_memory) {
                result = backend->driver.host_register(
                    backend->driver.user_data,
                    registration->data,
                    registration->bytes);
                host_registered =
                    result == CudaDriverResult::success;
            }
            if (result != CudaDriverResult::success &&
                allocate_device_address &&
                address != 0) {
                const auto free_result = backend->driver.mem_free(
                    backend->driver.user_data,
                    address);
                if (free_result == CudaDriverResult::success) {
                    address = 0;
                } else if (
                    free_result == CudaDriverResult::context_lost) {
                    result = free_result;
                }
            }
            if (context_pushed) {
                const auto pop_result = backend->pop_context();
                result = combine_with_pop(result, pop_result);
            }
            if (result != CudaDriverResult::success ||
                address == 0) {
                if (host_registered) {
                    const auto push_result = backend->push_context();
                    if (push_result == CudaDriverResult::success) {
                        auto cleanup_result =
                            backend->driver.host_unregister(
                            backend->driver.user_data,
                            registration->data);
                        if (cleanup_result ==
                            CudaDriverResult::success) {
                            host_registered = false;
                        }
                        if (cleanup_result ==
                                CudaDriverResult::success &&
                            allocate_device_address &&
                            address != 0) {
                            cleanup_result =
                                backend->driver.mem_free(
                                backend->driver.user_data,
                                address);
                            if (cleanup_result ==
                                CudaDriverResult::success) {
                                address = 0;
                            }
                        }
                        cleanup_result = combine_with_pop(
                            cleanup_result,
                            backend->pop_context());
                        if (cleanup_result ==
                            CudaDriverResult::context_lost) {
                            result = cleanup_result;
                        }
                    } else if (
                        push_result ==
                        CudaDriverResult::context_lost) {
                        result = push_result;
                    }
                }
                const bool residual_ownership =
                    host_registered ||
                    (allocate_device_address && address != 0);
                if (residual_ownership) {
                    auto& pending = backend->buffers[free_index];
                    pending.host_data = registration->data;
                    pending.device_address = address;
                    pending.bytes = registration->bytes;
                    pending.flags = registration->flags;
                    pending.token =
                        static_cast<std::uint64_t>(free_index + 1);
                    pending.registered = true;
                    pending.owns_device_address =
                        allocate_device_address && address != 0;
                    pending.owns_host_registration =
                        host_registered;
                    std::copy_n(
                        registration->name,
                        RTFW_DEVICE_IDENTIFIER_CAPACITY,
                        pending.name.begin());
                }
                auto status =
                    result == CudaDriverResult::success
                    ? RTFW_DEVICE_STATUS_INTERNAL_ERROR
                    : device_status(result);
                if (residual_ownership &&
                    status != RTFW_DEVICE_STATUS_LOST) {
                    status = RTFW_DEVICE_STATUS_RESET_REQUIRED;
                }
                backend->set_health_after(status);
                return status;
            }
            owned = allocate_device_address;
        }

        auto& buffer = backend->buffers[free_index];
        buffer.host_data = registration->data;
        buffer.device_address = address;
        buffer.bytes = registration->bytes;
        buffer.flags = registration->flags;
        buffer.token = static_cast<std::uint64_t>(free_index + 1);
        buffer.registered = true;
        buffer.owns_device_address = owned;
        buffer.owns_host_registration = host_registered;
        std::copy_n(
            registration->name,
            RTFW_DEVICE_IDENTIFIER_CAPACITY,
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
        for (std::size_t slot_index = 0;
             slot_index < backend->config.queue_capacity;
             ++slot_index) {
            const auto& slot = backend->slots[slot_index];
            if (slot.state.load(std::memory_order_acquire) ==
                kSlotFree) {
                continue;
            }
            for (std::size_t reference_index = 0;
                 reference_index < slot.buffer_count;
                 ++reference_index) {
                if (slot.buffer_tokens[reference_index] == token) {
                    return RTFW_DEVICE_STATUS_INVALID_STATE;
                }
            }
        }
        if (buffer->owns_device_address ||
            buffer->owns_host_registration) {
            auto result = backend->push_context();
            if (result == CudaDriverResult::success) {
                if (buffer->owns_host_registration) {
                    result = backend->driver.host_unregister(
                        backend->driver.user_data,
                        buffer->host_data);
                    if (result == CudaDriverResult::success) {
                        buffer->owns_host_registration = false;
                    }
                }
                if (result == CudaDriverResult::success &&
                    buffer->owns_device_address) {
                    result = backend->driver.mem_free(
                        backend->driver.user_data,
                        buffer->device_address);
                    if (result == CudaDriverResult::success) {
                        buffer->owns_device_address = false;
                    }
                }
                const auto pop_result = backend->pop_context();
                result = combine_with_pop(result, pop_result);
            }
            if (result != CudaDriverResult::success) {
                const auto status = device_status(result);
                backend->set_health_after(status);
                return status;
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
        const auto health =
            backend->health_state.load(std::memory_order_acquire);
        if (health == RTFW_DEVICE_HEALTH_LOST) {
            return RTFW_DEVICE_STATUS_LOST;
        }
        if (health == RTFW_DEVICE_HEALTH_RESET_REQUIRED) {
            return RTFW_DEVICE_STATUS_RESET_REQUIRED;
        }
        if (!backend->submission_valid(*submission)) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }

        auto* slot = backend->acquire_slot();
        if (!slot) {
            backend->queue_rejections.fetch_add(
                1,
                std::memory_order_relaxed);
            return RTFW_DEVICE_STATUS_QUEUE_FULL;
        }
        slot->submission_id = submission->submission_id;
        slot->started_ns = backend->driver.monotonic_time_ns(
            backend->driver.user_data);
        slot->timeout_ns = submission->timeout_ns;
        slot->buffer_count = submission->buffer_count;
        slot->timed_out = false;
        std::fill(
            slot->buffer_tokens.begin(),
            slot->buffer_tokens.end(),
            0);
        for (std::size_t index = 0;
             index < submission->buffer_count;
             ++index) {
            slot->buffer_tokens[index] =
                submission->buffers[index].buffer_token;
        }

        auto result = backend->push_context();
        bool operation_attempted = false;
        if (result == CudaDriverResult::success) {
            operation_attempted = true;
            result = backend->execute(*submission, slot->stream);
            if (result == CudaDriverResult::success) {
                result = backend->driver.event_record(
                    backend->driver.user_data,
                    slot->event,
                    slot->stream);
            }
            const auto pop_result = backend->pop_context();
            result = combine_with_pop(result, pop_result);
        }
        if (result != CudaDriverResult::success) {
            const auto status = device_status(result);
            backend->set_health_after(
                operation_attempted &&
                        status != RTFW_DEVICE_STATUS_LOST
                    ? RTFW_DEVICE_STATUS_RESET_REQUIRED
                    : status);
            if (operation_attempted) {
                backend->outstanding.fetch_add(
                    1,
                    std::memory_order_relaxed);
                slot->state.store(
                    kSlotQuarantined,
                    std::memory_order_release);
            } else {
                slot->state.store(
                    kSlotFree,
                    std::memory_order_release);
            }
            return status;
        }

        backend->submissions.fetch_add(
            1,
            std::memory_order_relaxed);
        backend->outstanding.fetch_add(
            1,
            std::memory_order_relaxed);
        slot->state.store(kSlotPending, std::memory_order_release);
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
        auto result = backend->push_context();
        if (result != CudaDriverResult::success) {
            auto status = device_status(result);
            if (status != RTFW_DEVICE_STATUS_LOST) {
                status = RTFW_DEVICE_STATUS_RESET_REQUIRED;
            }
            backend->set_health_after(status);
            return status;
        }

        auto poll_result = CudaDriverResult::success;
        const auto now = backend->driver.monotonic_time_ns(
            backend->driver.user_data);
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
            result = backend->driver.event_query(
                backend->driver.user_data,
                slot.event);
            if (result == CudaDriverResult::not_ready) {
                if (now >= slot.started_ns &&
                    now - slot.started_ns >= slot.timeout_ns) {
                    slot.timed_out = true;
                }
                slot.state.store(
                    kSlotPending,
                    std::memory_order_release);
                continue;
            }

            if (result != CudaDriverResult::success &&
                result != CudaDriverResult::context_lost) {
                slot.state.store(
                    kSlotQuarantined,
                    std::memory_order_release);
                backend->set_health_after(
                    RTFW_DEVICE_STATUS_RESET_REQUIRED);
                poll_result = result;
                break;
            }

            auto status = device_status(result);
            if (result == CudaDriverResult::success &&
                slot.timed_out) {
                status = RTFW_DEVICE_STATUS_TIMEOUT;
            }
            auto& completion = output[*out_count];
            completion = {};
            completion.struct_size = sizeof(completion);
            completion.status = status;
            completion.submission_id = slot.submission_id;
            completion.device_timestamp_ns = now;
            completion.value = static_cast<std::uint64_t>(index);
            ++*out_count;

            backend->set_health_after(status);
            backend->completions.fetch_add(
                1,
                std::memory_order_relaxed);
            if (status == RTFW_DEVICE_STATUS_LOST) {
                slot.state.store(
                    kSlotQuarantined,
                    std::memory_order_release);
            } else {
                backend->outstanding.fetch_sub(
                    1,
                    std::memory_order_relaxed);
                slot.state.store(
                    kSlotFree,
                    std::memory_order_release);
            }
        }

        result = combine_with_pop(
            poll_result,
            backend->pop_context());
        if (result != CudaDriverResult::success) {
            *out_count = 0;
            auto status = device_status(result);
            if (status != RTFW_DEVICE_STATUS_LOST) {
                status = RTFW_DEVICE_STATUS_RESET_REQUIRED;
            }
            backend->set_health_after(status);
            return status;
        }
        if (backend->health_state.load(std::memory_order_acquire) ==
            RTFW_DEVICE_HEALTH_RESET_REQUIRED) {
            *out_count = 0;
            return RTFW_DEVICE_STATUS_RESET_REQUIRED;
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
        output->cancellations = 0;
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
        if (backend->health_state.load(std::memory_order_acquire) ==
            RTFW_DEVICE_HEALTH_LOST) {
            return RTFW_DEVICE_STATUS_LOST;
        }
        for (std::size_t index = 0;
             index < backend->config.queue_capacity;
             ++index) {
            const auto state = backend->slots[index].state.load(
                std::memory_order_acquire);
            if (state != kSlotFree &&
                state != kSlotQuarantined) {
                return RTFW_DEVICE_STATUS_INVALID_STATE;
            }
        }

        auto result = backend->push_context();
        if (result != CudaDriverResult::success) {
            auto status = device_status(result);
            if (status != RTFW_DEVICE_STATUS_LOST) {
                status = RTFW_DEVICE_STATUS_RESET_REQUIRED;
            }
            backend->set_health_after(status);
            return status;
        }
        for (std::size_t index = 0;
             index < backend->config.queue_capacity;
             ++index) {
            auto& slot = backend->slots[index];
            if (slot.state.load(std::memory_order_acquire) !=
                kSlotQuarantined) {
                continue;
            }
            result = backend->driver.stream_synchronize(
                backend->driver.user_data,
                slot.stream);
            if (result != CudaDriverResult::success) {
                break;
            }
            slot.state.store(kSlotFree, std::memory_order_release);
            backend->outstanding.fetch_sub(
                1,
                std::memory_order_relaxed);
        }
        const auto pop_result = backend->pop_context();
        result = combine_with_pop(result, pop_result);
        if (result != CudaDriverResult::success) {
            auto status = device_status(result);
            if (status != RTFW_DEVICE_STATUS_LOST) {
                status = RTFW_DEVICE_STATUS_RESET_REQUIRED;
            }
            backend->set_health_after(status);
            return status;
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

        auto first_failure = CudaDriverResult::success;
        auto result = backend->push_context();
        if (result != CudaDriverResult::success) {
            first_failure = result;
        } else {
            for (std::size_t index = 0;
                 index < backend->config.queue_capacity;
                 ++index) {
                auto& slot = backend->slots[index];
                const auto state =
                    slot.state.load(std::memory_order_acquire);
                if (state == kSlotPending) {
                    result = backend->driver.event_synchronize(
                        backend->driver.user_data,
                        slot.event);
                } else if (state == kSlotQuarantined) {
                    result = backend->driver.stream_synchronize(
                        backend->driver.user_data,
                        slot.stream);
                } else if (state == kSlotFree) {
                    result = CudaDriverResult::success;
                } else {
                    result = CudaDriverResult::error;
                }
                if (result != CudaDriverResult::success) {
                    first_failure = result;
                    break;
                }
                if (state == kSlotPending ||
                    state == kSlotQuarantined) {
                    backend->outstanding.fetch_sub(
                        1,
                        std::memory_order_relaxed);
                }
                slot.state.store(kSlotFree, std::memory_order_release);
            }

            if (first_failure == CudaDriverResult::success) {
                backend->outstanding.store(
                    0,
                    std::memory_order_relaxed);
                for (std::size_t index = 0;
                     index < backend->config.buffer_capacity;
                     ++index) {
                    auto& buffer = backend->buffers[index];
                    if (buffer.registered &&
                        buffer.owns_host_registration) {
                        result = backend->driver.host_unregister(
                            backend->driver.user_data,
                            buffer.host_data);
                        if (result != CudaDriverResult::success) {
                            first_failure = result;
                            break;
                        }
                        buffer.owns_host_registration = false;
                    }
                    if (buffer.registered &&
                        buffer.owns_device_address) {
                        result = backend->driver.mem_free(
                            backend->driver.user_data,
                            buffer.device_address);
                        if (result != CudaDriverResult::success) {
                            first_failure = result;
                            break;
                        }
                        buffer.owns_device_address = false;
                    }
                    buffer = {};
                }
            }

            if (first_failure == CudaDriverResult::success) {
                for (std::size_t index = 0;
                     index < backend->config.queue_capacity;
                     ++index) {
                    auto& slot = backend->slots[index];
                    if (slot.event != 0) {
                        result = backend->driver.event_destroy(
                            backend->driver.user_data,
                            slot.event);
                        if (result != CudaDriverResult::success) {
                            first_failure = result;
                            break;
                        }
                        slot.event = 0;
                    }
                }
            }
            result = backend->pop_context();
            if (result == CudaDriverResult::context_lost ||
                (first_failure == CudaDriverResult::success &&
                 result != CudaDriverResult::success)) {
                first_failure = result;
            }
        }
        if (first_failure != CudaDriverResult::success) {
            auto status = device_status(first_failure);
            if (status != RTFW_DEVICE_STATUS_LOST) {
                status = RTFW_DEVICE_STATUS_RESET_REQUIRED;
            }
            backend->set_health_after(status);
            backend->shutdown_active.store(
                false,
                std::memory_order_release);
            return status;
        }
        backend->last_status.store(
            RTFW_DEVICE_STATUS_OK,
            std::memory_order_release);
        backend->health_state.store(
            RTFW_DEVICE_HEALTH_SHUTDOWN,
            std::memory_order_release);
        backend->shutdown_incomplete.store(
            false,
            std::memory_order_release);
        backend->shutdown_active.store(
            false,
            std::memory_order_release);
        return RTFW_DEVICE_STATUS_OK;
    }

    CudaDriverApi driver;
    CudaBackendConfig config;
    std::unique_ptr<CudaStream[]> streams;
    std::size_t stream_count = 0;
    std::unique_ptr<Slot[]> slots;
    std::unique_ptr<Buffer[]> buffers;
    std::unique_ptr<Binding[]> bindings;
    std::unique_ptr<Kernel[]> kernels;
    std::atomic<bool> initialized{false};
    std::atomic<bool> shutdown_incomplete{false};
    std::atomic<bool> shutdown_active{false};
    std::atomic<bool> permanent_loss{false};
    std::atomic<std::size_t> slot_hint{0};
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

CudaDeviceBackend::CudaDeviceBackend(
    const CudaDriverApi& driver,
    const CudaBackendConfig& config)
    : impl_(std::make_unique<Impl>(driver, config)) {}

CudaDeviceBackend::~CudaDeviceBackend() = default;
CudaDeviceBackend::CudaDeviceBackend(
    CudaDeviceBackend&&) noexcept = default;
CudaDeviceBackend& CudaDeviceBackend::operator=(
    CudaDeviceBackend&&) noexcept = default;

rtfw_device_backend_api CudaDeviceBackend::api() noexcept {
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

rtfw_device_status CudaDeviceBackend::bind_device_buffer(
    std::string_view name,
    CudaDeviceAddress address,
    std::uint64_t bytes) noexcept {
    if (!impl_ ||
        impl_->initialized.load(std::memory_order_acquire)) {
        return RTFW_DEVICE_STATUS_INVALID_STATE;
    }
    if (!valid_identifier(
            name,
            RTFW_DEVICE_IDENTIFIER_CAPACITY) ||
        !valid_device_range(address, bytes)) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    std::size_t free_index = impl_->config.buffer_capacity;
    for (std::size_t index = 0;
         index < impl_->config.buffer_capacity;
         ++index) {
        auto& binding = impl_->bindings[index];
        if (binding.bound) {
            if (std::string_view(binding.name.data()) == name) {
                return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
            }
        } else if (free_index == impl_->config.buffer_capacity) {
            free_index = index;
        }
    }
    if (free_index == impl_->config.buffer_capacity) {
        return RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED;
    }
    auto& binding = impl_->bindings[free_index];
    binding.device_address = address;
    binding.bytes = bytes;
    binding.bound = true;
    std::copy(name.begin(), name.end(), binding.name.begin());
    binding.name[name.size()] = '\0';
    return RTFW_DEVICE_STATUS_OK;
}

rtfw_device_status CudaDeviceBackend::register_kernel(
    CudaFunction function,
    std::uint64_t& out_kernel_token) noexcept {
    out_kernel_token = 0;
    if (!impl_ ||
        impl_->initialized.load(std::memory_order_acquire)) {
        return RTFW_DEVICE_STATUS_INVALID_STATE;
    }
    if (function == 0) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    for (std::size_t index = 0;
         index < impl_->config.kernel_capacity;
         ++index) {
        auto& kernel = impl_->kernels[index];
        if (kernel.registered && kernel.function == function) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        if (!kernel.registered) {
            kernel.function = function;
            kernel.token = static_cast<std::uint64_t>(index + 1);
            kernel.registered = true;
            out_kernel_token = kernel.token;
            return RTFW_DEVICE_STATUS_OK;
        }
    }
    return RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED;
}

} // namespace rt

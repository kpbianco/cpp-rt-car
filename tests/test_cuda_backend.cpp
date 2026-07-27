#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <thread>
#include <vector>

#include <rt/cuda_backend.hpp>
#include <rt/runtime.hpp>

namespace {

class FakeCudaDriver {
public:
    static constexpr rt::CudaContext context = 0xc001u;
    static constexpr rt::CudaFunction add_one_function = 0xf001u;
    static constexpr std::size_t event_capacity = 32;
    static constexpr std::size_t allocation_capacity = 8;
    static constexpr std::size_t allocation_bytes = 4096;

    rt::CudaDriverApi api() noexcept {
        rt::CudaDriverApi result{};
        result.user_data = this;
        result.push_context = &push_context;
        result.pop_context = &pop_context;
        result.event_create = &event_create;
        result.event_destroy = &event_destroy;
        result.event_record = &event_record;
        result.event_query = &event_query;
        result.event_synchronize = &event_synchronize;
        result.stream_synchronize = &stream_synchronize;
        result.mem_alloc = &mem_alloc;
        result.mem_free = &mem_free;
        result.host_register = &host_register;
        result.host_unregister = &host_unregister;
        result.memcpy_host_to_device_async =
            &memcpy_host_to_device_async;
        result.memcpy_device_to_host_async =
            &memcpy_device_to_host_async;
        result.memcpy_device_to_device_async =
            &memcpy_device_to_device_async;
        result.memset_d8_async = &memset_d8_async;
        result.launch_kernel = &launch_kernel;
        result.monotonic_time_ns = &monotonic_time_ns;
        return result;
    }

    void make_events_ready() noexcept {
        for (auto& event : events_) {
            if (event.recorded.load(std::memory_order_acquire)) {
                event.ready.store(true, std::memory_order_release);
            }
        }
    }

    void advance(std::uint64_t nanoseconds) noexcept {
        now_ns_.fetch_add(nanoseconds, std::memory_order_relaxed);
    }

    std::atomic<bool> complete_on_record{false};
    std::atomic<bool> fail_next_event_record{false};
    std::atomic<bool> fail_next_event_query{false};
    std::atomic<bool> fail_next_event_sync{false};
    std::atomic<bool> fail_next_stream_sync{false};
    std::atomic<bool> fail_next_mem_free{false};
    std::atomic<bool> fail_next_host_register{false};
    std::atomic<bool> lose_context_on_query{false};
    std::atomic<std::uint64_t> event_syncs{0};
    std::atomic<std::uint64_t> stream_syncs{0};
    std::atomic<std::uint64_t> allocations{0};
    std::atomic<std::uint64_t> frees{0};
    std::atomic<std::uint64_t> host_registrations{0};
    std::atomic<std::uint64_t> host_unregistrations{0};
    std::atomic<std::uint64_t> launches{0};

private:
    struct EventState {
        std::atomic<bool> allocated{false};
        std::atomic<bool> recorded{false};
        std::atomic<bool> ready{false};
    };

    struct Allocation {
        bool allocated = false;
        alignas(std::max_align_t)
            std::array<std::byte, allocation_bytes> bytes{};
    };

    static FakeCudaDriver* self(void* user_data) noexcept {
        return static_cast<FakeCudaDriver*>(user_data);
    }

    static EventState* event_for(
        FakeCudaDriver& driver,
        rt::CudaEvent event) noexcept {
        if (event == 0 || event > driver.events_.size()) {
            return nullptr;
        }
        auto& state = driver.events_[
            static_cast<std::size_t>(event - 1)];
        return state.allocated.load(std::memory_order_acquire)
            ? &state
            : nullptr;
    }

    static rt::CudaDriverResult push_context(
        void* user_data,
        rt::CudaContext requested) noexcept {
        return user_data && requested == context
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::context_lost;
    }

    static rt::CudaDriverResult pop_context(
        void* user_data,
        rt::CudaContext* out_context) noexcept {
        if (!user_data || !out_context) {
            return rt::CudaDriverResult::invalid_value;
        }
        *out_context = context;
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult event_create(
        void* user_data,
        rt::CudaEvent* out_event) noexcept {
        auto* driver = self(user_data);
        if (!driver || !out_event) {
            return rt::CudaDriverResult::invalid_value;
        }
        for (std::size_t index = 0;
             index < driver->events_.size();
             ++index) {
            auto expected = false;
            if (driver->events_[index].allocated.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                driver->events_[index].recorded.store(
                    false,
                    std::memory_order_relaxed);
                driver->events_[index].ready.store(
                    false,
                    std::memory_order_relaxed);
                *out_event = static_cast<rt::CudaEvent>(index + 1);
                return rt::CudaDriverResult::success;
            }
        }
        return rt::CudaDriverResult::out_of_memory;
    }

    static rt::CudaDriverResult event_destroy(
        void* user_data,
        rt::CudaEvent event) noexcept {
        auto* driver = self(user_data);
        auto* state =
            driver ? event_for(*driver, event) : nullptr;
        if (!state) {
            return rt::CudaDriverResult::invalid_value;
        }
        state->recorded.store(false, std::memory_order_relaxed);
        state->ready.store(false, std::memory_order_relaxed);
        state->allocated.store(false, std::memory_order_release);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult event_record(
        void* user_data,
        rt::CudaEvent event,
        rt::CudaStream) noexcept {
        auto* driver = self(user_data);
        auto* state =
            driver ? event_for(*driver, event) : nullptr;
        if (!state) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->fail_next_event_record.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::error;
        }
        state->recorded.store(true, std::memory_order_release);
        state->ready.store(
            driver->complete_on_record.load(
                std::memory_order_acquire),
            std::memory_order_release);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult event_query(
        void* user_data,
        rt::CudaEvent event) noexcept {
        auto* driver = self(user_data);
        auto* state =
            driver ? event_for(*driver, event) : nullptr;
        if (!state) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->lose_context_on_query.load(
                std::memory_order_acquire)) {
            return rt::CudaDriverResult::context_lost;
        }
        if (driver->fail_next_event_query.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::launch_failure;
        }
        return state->ready.load(std::memory_order_acquire)
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::not_ready;
    }

    static rt::CudaDriverResult event_synchronize(
        void* user_data,
        rt::CudaEvent event) noexcept {
        auto* driver = self(user_data);
        auto* state =
            driver ? event_for(*driver, event) : nullptr;
        if (!state) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->fail_next_event_sync.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::launch_failure;
        }
        state->ready.store(true, std::memory_order_release);
        driver->event_syncs.fetch_add(1, std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult stream_synchronize(
        void* user_data,
        rt::CudaStream) noexcept {
        auto* driver = self(user_data);
        if (!driver) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->fail_next_stream_sync.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::launch_failure;
        }
        driver->make_events_ready();
        driver->stream_syncs.fetch_add(1, std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult mem_alloc(
        void* user_data,
        std::uint64_t bytes,
        rt::CudaDeviceAddress* out_address) noexcept {
        auto* driver = self(user_data);
        if (!driver || !out_address ||
            bytes == 0 || bytes > allocation_bytes) {
            return rt::CudaDriverResult::invalid_value;
        }
        for (auto& allocation : driver->memory_) {
            if (!allocation.allocated) {
                allocation.allocated = true;
                *out_address =
                    reinterpret_cast<std::uintptr_t>(
                        allocation.bytes.data());
                driver->allocations.fetch_add(
                    1,
                    std::memory_order_relaxed);
                return rt::CudaDriverResult::success;
            }
        }
        return rt::CudaDriverResult::out_of_memory;
    }

    static rt::CudaDriverResult mem_free(
        void* user_data,
        rt::CudaDeviceAddress address) noexcept {
        auto* driver = self(user_data);
        if (!driver || address == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->fail_next_mem_free.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::launch_failure;
        }
        for (auto& allocation : driver->memory_) {
            if (reinterpret_cast<std::uintptr_t>(
                    allocation.bytes.data()) == address &&
                allocation.allocated) {
                allocation.allocated = false;
                driver->frees.fetch_add(
                    1,
                    std::memory_order_relaxed);
                return rt::CudaDriverResult::success;
            }
        }
        return rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult host_register(
        void* user_data,
        void* address,
        std::uint64_t bytes) noexcept {
        auto* driver = self(user_data);
        if (!driver || !address || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->fail_next_host_register.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::error;
        }
        driver->host_registrations.fetch_add(
            1,
            std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult host_unregister(
        void* user_data,
        void* address) noexcept {
        auto* driver = self(user_data);
        if (!driver || !address) {
            return rt::CudaDriverResult::invalid_value;
        }
        driver->host_unregistrations.fetch_add(
            1,
            std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult memcpy_host_to_device_async(
        void*,
        rt::CudaDeviceAddress destination,
        const void* source,
        std::uint64_t bytes,
        rt::CudaStream) noexcept {
        if (destination == 0 || !source || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        std::memcpy(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(destination)),
            source,
            static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult memcpy_device_to_host_async(
        void*,
        void* destination,
        rt::CudaDeviceAddress source,
        std::uint64_t bytes,
        rt::CudaStream) noexcept {
        if (!destination || source == 0 || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        std::memcpy(
            destination,
            reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(source)),
            static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult memcpy_device_to_device_async(
        void*,
        rt::CudaDeviceAddress destination,
        rt::CudaDeviceAddress source,
        std::uint64_t bytes,
        rt::CudaStream) noexcept {
        if (destination == 0 || source == 0 || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        std::memmove(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(destination)),
            reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(source)),
            static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult memset_d8_async(
        void*,
        rt::CudaDeviceAddress destination,
        std::uint8_t value,
        std::uint64_t bytes,
        rt::CudaStream) noexcept {
        if (destination == 0 || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        std::memset(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(destination)),
            value,
            static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult launch_kernel(
        void* user_data,
        rt::CudaFunction function,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        rt::CudaStream,
        void* const* arguments) noexcept {
        auto* driver = self(user_data);
        if (!driver || function != add_one_function ||
            !arguments || !arguments[0] || !arguments[1]) {
            return rt::CudaDriverResult::invalid_value;
        }
        rt::CudaDeviceAddress address = 0;
        std::uint32_t count = 0;
        std::memcpy(&address, arguments[0], sizeof(address));
        std::memcpy(&count, arguments[1], sizeof(count));
        auto* values = reinterpret_cast<std::int32_t*>(
            static_cast<std::uintptr_t>(address));
        for (std::uint32_t index = 0; index < count; ++index) {
            ++values[index];
        }
        driver->launches.fetch_add(1, std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static std::uint64_t monotonic_time_ns(
        void* user_data) noexcept {
        auto* driver = self(user_data);
        return driver
            ? driver->now_ns_.load(std::memory_order_relaxed)
            : 0;
    }

    std::array<EventState, event_capacity> events_{};
    std::array<Allocation, allocation_capacity> memory_{};
    std::atomic<std::uint64_t> now_ns_{1};
};

rt::CudaBackendConfig config(
    std::span<const rt::CudaStream> streams,
    std::size_t queue_capacity = 4,
    std::size_t buffer_capacity = 4) {
    rt::CudaBackendConfig result{};
    result.queue_capacity = queue_capacity;
    result.buffer_capacity = buffer_capacity;
    result.kernel_capacity = 4;
    result.context = FakeCudaDriver::context;
    result.streams = streams;
    return result;
}

rtfw_device_status initialize(
    rtfw_device_backend_api& api,
    std::size_t queue_capacity,
    std::size_t buffer_capacity) {
    rtfw_device_init_config requested{};
    requested.struct_size = sizeof(requested);
    requested.abi_version = RTFW_DEVICE_ABI_VERSION;
    requested.requested_in_flight = queue_capacity;
    requested.requested_registered_buffers = buffer_capacity;
    return api.initialize(api.instance, &requested);
}

std::uint64_t register_buffer(
    rtfw_device_backend_api& api,
    const char* name,
    void* data,
    std::size_t bytes) {
    rtfw_device_buffer_registration registration{};
    registration.struct_size = sizeof(registration);
    registration.flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    registration.data = data;
    registration.bytes = bytes;
    std::copy_n(
        name,
        std::strlen(name) + 1,
        registration.name);
    std::uint64_t token = 0;
    EXPECT_EQ(
        api.register_buffer(
            api.instance,
            &registration,
            &token),
        RTFW_DEVICE_STATUS_OK);
    return token;
}

rt::DeviceSubmission submission(
    std::uint64_t id,
    std::uint32_t opcode,
    std::uint64_t timeout_ns = 1000) {
    auto result = rt::make_device_submission();
    result.submission_id = id;
    result.timeout_ns = timeout_ns;
    result.opcode = opcode;
    return result;
}

rtfw_device_completion poll_one(
    rtfw_device_backend_api& api,
    std::uint64_t& count) {
    rtfw_device_completion completion{};
    count = 0;
    EXPECT_EQ(
        api.poll(api.instance, &completion, 1, &count),
        RTFW_DEVICE_STATUS_OK);
    return completion;
}

void complete_submission(
    rtfw_device_backend_api& api,
    rt::DeviceSubmission& requested) {
    ASSERT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_OK);
    std::uint64_t count = 0;
    const auto completion = poll_one(api, count);
    ASSERT_EQ(count, 1u);
    ASSERT_EQ(completion.submission_id, requested.submission_id);
    ASSERT_EQ(completion.status, RTFW_DEVICE_STATUS_OK);
}

rt::CallbackResult prepare_cuda_noop(
    void*,
    const rt::DeviceCallbackContext&,
    rt::DeviceSubmission& requested) {
    requested.timeout_ns = 1'000'000;
    requested.opcode = rt::cuda_device_opcode_noop;
    return rt::CallbackResult::ok;
}

rt::CallbackResult mark_cuda_dependent(
    void* user_data,
    const rt::CallbackContext&) {
    auto& called = *static_cast<std::atomic<bool>*>(user_data);
    called.store(true, std::memory_order_release);
    return rt::CallbackResult::ok;
}

TEST(CudaBackend, RejectsMalformedDriverAndSetup) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    auto driver_api = driver.api();
    driver_api.event_query = nullptr;
    EXPECT_THROW(
        (void)rt::CudaDeviceBackend(
            driver_api,
            config(streams)),
        std::invalid_argument);

    driver_api = driver.api();
    rt::CudaDeviceBackend backend(driver_api, config(streams));
    auto api = backend.api();
    EXPECT_EQ(initialize(api, 4, 4), RTFW_DEVICE_STATUS_OK);
    std::array<std::byte, 16> bytes{};
    rtfw_device_buffer_registration registration{};
    registration.struct_size = sizeof(registration);
    registration.flags = RTFW_DEVICE_BUFFER_DEVICE_READ;
    registration.data = bytes.data();
    registration.bytes = bytes.size();
    std::memcpy(registration.name, "bad name", sizeof("bad name"));
    std::uint64_t token = 0;
    EXPECT_EQ(
        api.register_buffer(
            api.instance,
            &registration,
            &token),
        RTFW_DEVICE_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, BoundedQueueSaturatesAndTimeoutQuarantinesUntilReady) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 0), RTFW_DEVICE_STATUS_OK);

    auto first = submission(
        1,
        rt::cuda_device_opcode_noop,
        10);
    auto second = submission(
        2,
        rt::cuda_device_opcode_noop,
        10);
    ASSERT_EQ(
        api.submit(api.instance, &first),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        api.submit(api.instance, &second),
        RTFW_DEVICE_STATUS_QUEUE_FULL);

    driver.advance(11);
    std::uint64_t count = 0;
    (void)poll_one(api, count);
    EXPECT_EQ(count, 0u);
    auto health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.outstanding, 1u);
    EXPECT_EQ(health.timeouts, 0u);

    driver.make_events_ready();
    const auto completion = poll_one(api, count);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(completion.submission_id, 1u);
    EXPECT_EQ(completion.status, RTFW_DEVICE_STATUS_TIMEOUT);
    health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_DEGRADED);
    EXPECT_EQ(health.outstanding, 0u);
    EXPECT_EQ(health.timeouts, 1u);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, CopiesLaunchesKernelAndReturnsDeviceData) {
    FakeCudaDriver driver;
    driver.complete_on_record.store(true, std::memory_order_release);
    const std::array<rt::CudaStream, 2> streams{0x51u, 0x52u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams));
    std::uint64_t kernel_token = 0;
    ASSERT_EQ(
        backend.register_kernel(
            FakeCudaDriver::add_one_function,
            kernel_token),
        RTFW_DEVICE_STATUS_OK);
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 4, 1), RTFW_DEVICE_STATUS_OK);

    std::array<std::int32_t, 8> values{
        1, 2, 3, 4, 5, 6, 7, 8};
    const auto buffer_token = register_buffer(
        api,
        "values",
        values.data(),
        sizeof(values));

    auto upload = submission(
        1,
        rt::cuda_device_opcode_copy_host_to_device);
    upload.buffer_count = 1;
    upload.buffers[0].buffer_token = buffer_token;
    upload.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    upload.buffers[0].bytes = sizeof(values);
    complete_submission(api, upload);

    rt::CudaKernelLaunch launch{};
    launch.kernel_token = kernel_token;
    launch.grid_x = 1;
    launch.block_x = 8;
    ASSERT_TRUE(rt::cuda_kernel_add_buffer_argument(launch, 0));
    const auto value_count =
        static_cast<std::uint32_t>(values.size());
    ASSERT_TRUE(rt::cuda_kernel_add_scalar_argument(
        launch,
        value_count));
    auto kernel = submission(
        2,
        rt::cuda_device_opcode_launch_kernel);
    kernel.buffer_count = 1;
    kernel.buffers[0].buffer_token = buffer_token;
    kernel.buffers[0].access = RTFW_DEVICE_ACCESS_READ_WRITE;
    kernel.buffers[0].bytes = sizeof(values);
    rt::set_cuda_kernel_launch(kernel, launch);
    complete_submission(api, kernel);

    std::fill(values.begin(), values.end(), 0);
    auto download = submission(
        3,
        rt::cuda_device_opcode_copy_device_to_host);
    download.buffer_count = 1;
    download.buffers[0].buffer_token = buffer_token;
    download.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
    download.buffers[0].bytes = sizeof(values);
    complete_submission(api, download);

    EXPECT_EQ(
        values,
        (std::array<std::int32_t, 8>{
            2, 3, 4, 5, 6, 7, 8, 9}));
    EXPECT_EQ(driver.launches.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(
        api.unregister_buffer(api.instance, buffer_token),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        driver.allocations.load(std::memory_order_acquire),
        driver.frees.load(std::memory_order_acquire));
    EXPECT_EQ(
        driver.host_registrations.load(std::memory_order_acquire),
        driver.host_unregistrations.load(std::memory_order_acquire));
}

TEST(CudaBackend, DeviceCopyAndMemsetValidateAndPreserveRanges) {
    FakeCudaDriver driver;
    driver.complete_on_record.store(true, std::memory_order_release);
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 4, 2));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 4, 2), RTFW_DEVICE_STATUS_OK);

    std::array<std::byte, 16> source{};
    std::array<std::byte, 16> destination{};
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<std::byte>(index + 1);
    }
    const auto source_token = register_buffer(
        api,
        "copy.source",
        source.data(),
        source.size());
    const auto destination_token = register_buffer(
        api,
        "copy.destination",
        destination.data(),
        destination.size());

    auto upload = submission(
        1,
        rt::cuda_device_opcode_copy_host_to_device);
    upload.buffer_count = 1;
    upload.buffers[0].buffer_token = source_token;
    upload.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    upload.buffers[0].bytes = source.size();
    complete_submission(api, upload);

    auto fill = submission(2, rt::cuda_device_opcode_memset_d8);
    fill.payload_size = 1;
    fill.payload[0] = 0xa5u;
    fill.buffer_count = 1;
    fill.buffers[0].buffer_token = destination_token;
    fill.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
    fill.buffers[0].offset = 4;
    fill.buffers[0].bytes = 8;
    complete_submission(api, fill);

    auto download = submission(
        3,
        rt::cuda_device_opcode_copy_device_to_host);
    download.buffer_count = 1;
    download.buffers[0].buffer_token = destination_token;
    download.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
    download.buffers[0].bytes = destination.size();
    complete_submission(api, download);
    for (std::size_t index = 0; index < destination.size(); ++index) {
        EXPECT_EQ(
            destination[index],
            index >= 4 && index < 12
                ? std::byte{0xa5}
                : std::byte{0});
    }

    auto copy = submission(
        4,
        rt::cuda_device_opcode_copy_device_to_device);
    copy.buffer_count = 2;
    copy.buffers[0].buffer_token = source_token;
    copy.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    copy.buffers[0].bytes = source.size();
    copy.buffers[1].buffer_token = destination_token;
    copy.buffers[1].access = RTFW_DEVICE_ACCESS_WRITE;
    copy.buffers[1].bytes = destination.size();
    complete_submission(api, copy);

    std::fill(destination.begin(), destination.end(), std::byte{0});
    download.submission_id = 5;
    complete_submission(api, download);
    EXPECT_EQ(destination, source);

    auto out_of_range = submission(
        6,
        rt::cuda_device_opcode_memset_d8);
    out_of_range.payload_size = 1;
    out_of_range.payload[0] = 0xffu;
    out_of_range.buffer_count = 1;
    out_of_range.buffers[0].buffer_token = destination_token;
    out_of_range.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
    out_of_range.buffers[0].offset = destination.size() - 1;
    out_of_range.buffers[0].bytes = 2;
    EXPECT_EQ(
        api.submit(api.instance, &out_of_range),
        RTFW_DEVICE_STATUS_INVALID_ARGUMENT);

    auto overlapping = submission(
        7,
        rt::cuda_device_opcode_copy_device_to_device);
    overlapping.buffer_count = 2;
    overlapping.buffers[0].buffer_token = destination_token;
    overlapping.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    overlapping.buffers[0].bytes = 8;
    overlapping.buffers[1].buffer_token = destination_token;
    overlapping.buffers[1].access = RTFW_DEVICE_ACCESS_WRITE;
    overlapping.buffers[1].offset = 4;
    overlapping.buffers[1].bytes = 8;
    EXPECT_EQ(
        api.submit(api.instance, &overlapping),
        RTFW_DEVICE_STATUS_INVALID_ARGUMENT);

    EXPECT_EQ(
        api.unregister_buffer(api.instance, destination_token),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        api.unregister_buffer(api.instance, source_token),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, ExternalDeviceBindingRetainsCallerOwnership) {
    FakeCudaDriver driver;
    driver.complete_on_record.store(true, std::memory_order_release);
    const std::array<rt::CudaStream, 1> streams{0x51u};
    auto backend_config = config(streams, 2, 1);
    backend_config.allocate_device_mirrors = false;
    backend_config.register_host_memory = false;
    rt::CudaDeviceBackend backend(
        driver.api(),
        backend_config);

    EXPECT_EQ(
        backend.bind_device_buffer(
            "overflow",
            std::numeric_limits<rt::CudaDeviceAddress>::max() - 3,
            8),
        RTFW_DEVICE_STATUS_INVALID_ARGUMENT);

    std::array<std::byte, 16> host{};
    std::array<std::byte, 16> external_device{};
    for (std::size_t index = 0; index < host.size(); ++index) {
        host[index] = static_cast<std::byte>(0x20 + index);
    }
    ASSERT_EQ(
        backend.bind_device_buffer(
            "external",
            reinterpret_cast<std::uintptr_t>(
                external_device.data()),
            external_device.size()),
        RTFW_DEVICE_STATUS_OK);

    auto api = backend.api();
    ASSERT_EQ(initialize(api, 2, 1), RTFW_DEVICE_STATUS_OK);
    const auto token = register_buffer(
        api,
        "external",
        host.data(),
        host.size());
    auto upload = submission(
        1,
        rt::cuda_device_opcode_copy_host_to_device);
    upload.buffer_count = 1;
    upload.buffers[0].buffer_token = token;
    upload.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    upload.buffers[0].bytes = host.size();
    complete_submission(api, upload);

    EXPECT_EQ(external_device, host);
    EXPECT_EQ(
        api.unregister_buffer(api.instance, token),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(driver.allocations.load(), 0u);
    EXPECT_EQ(driver.frees.load(), 0u);
    EXPECT_EQ(driver.host_registrations.load(), 0u);
    EXPECT_EQ(driver.host_unregistrations.load(), 0u);
}

TEST(CudaBackend, FailedRegistrationRetainsOwnershipForShutdown) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 1), RTFW_DEVICE_STATUS_OK);

    std::array<std::byte, 32> bytes{};
    rtfw_device_buffer_registration registration{};
    registration.struct_size = sizeof(registration);
    registration.flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    registration.data = bytes.data();
    registration.bytes = bytes.size();
    std::memcpy(
        registration.name,
        "registration.failure",
        sizeof("registration.failure"));
    driver.fail_next_host_register.store(
        true,
        std::memory_order_release);
    driver.fail_next_mem_free.store(
        true,
        std::memory_order_release);
    std::uint64_t token = 99;
    EXPECT_EQ(
        api.register_buffer(
            api.instance,
            &registration,
            &token),
        RTFW_DEVICE_STATUS_RESET_REQUIRED);
    EXPECT_EQ(token, 0u);
    EXPECT_EQ(driver.allocations.load(), 1u);
    EXPECT_EQ(driver.frees.load(), 0u);

    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(driver.frees.load(), 1u);
    EXPECT_EQ(driver.host_registrations.load(), 0u);
    EXPECT_EQ(driver.host_unregistrations.load(), 0u);
}

TEST(CudaBackend, FailedEnqueueIsQuarantinedAndResetDrainsIt) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 0), RTFW_DEVICE_STATUS_OK);

    driver.fail_next_event_record.store(
        true,
        std::memory_order_release);
    auto requested = submission(
        1,
        rt::cuda_device_opcode_noop);
    EXPECT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_ERROR);
    auto health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_RESET_REQUIRED);
    EXPECT_EQ(health.outstanding, 1u);
    EXPECT_EQ(api.reset(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        driver.stream_syncs.load(std::memory_order_acquire),
        1u);

    health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_HEALTHY);
    EXPECT_EQ(health.outstanding, 0u);
    EXPECT_EQ(health.resets, 1u);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, QueryFailureRequiresSuccessfulDrainBeforeReuse) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 0), RTFW_DEVICE_STATUS_OK);

    auto requested = submission(
        1,
        rt::cuda_device_opcode_noop);
    ASSERT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_OK);
    driver.fail_next_event_query.store(
        true,
        std::memory_order_release);
    rtfw_device_completion completion{};
    std::uint64_t count = 9;
    EXPECT_EQ(
        api.poll(api.instance, &completion, 1, &count),
        RTFW_DEVICE_STATUS_RESET_REQUIRED);
    EXPECT_EQ(count, 0u);

    auto health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_RESET_REQUIRED);
    EXPECT_EQ(health.outstanding, 1u);

    driver.fail_next_stream_sync.store(
        true,
        std::memory_order_release);
    EXPECT_EQ(
        api.reset(api.instance),
        RTFW_DEVICE_STATUS_RESET_REQUIRED);
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_RESET_REQUIRED);
    EXPECT_EQ(health.outstanding, 1u);

    EXPECT_EQ(api.reset(api.instance), RTFW_DEVICE_STATUS_OK);
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_HEALTHY);
    EXPECT_EQ(health.outstanding, 0u);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, ShutdownDrainsOutstandingWorkBeforeFreeingBuffers) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 1), RTFW_DEVICE_STATUS_OK);
    std::array<std::byte, 32> bytes{};
    const auto token = register_buffer(
        api,
        "pending",
        bytes.data(),
        bytes.size());
    auto requested = submission(
        1,
        rt::cuda_device_opcode_copy_host_to_device);
    requested.buffer_count = 1;
    requested.buffers[0].buffer_token = token;
    requested.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    requested.buffers[0].bytes = bytes.size();
    ASSERT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        api.unregister_buffer(api.instance, token),
        RTFW_DEVICE_STATUS_INVALID_STATE);

    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        driver.event_syncs.load(std::memory_order_acquire),
        1u);
    EXPECT_EQ(driver.allocations.load(), 1u);
    EXPECT_EQ(driver.frees.load(), 1u);
}

TEST(CudaBackend, FailedShutdownRetainsResourcesAndCanBeRetried) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 1), RTFW_DEVICE_STATUS_OK);
    std::array<std::byte, 32> bytes{};
    const auto token = register_buffer(
        api,
        "shutdown.retry",
        bytes.data(),
        bytes.size());
    auto requested = submission(
        1,
        rt::cuda_device_opcode_copy_host_to_device);
    requested.buffer_count = 1;
    requested.buffers[0].buffer_token = token;
    requested.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    requested.buffers[0].bytes = bytes.size();
    ASSERT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_OK);

    driver.fail_next_event_sync.store(
        true,
        std::memory_order_release);
    EXPECT_EQ(
        api.shutdown(api.instance),
        RTFW_DEVICE_STATUS_RESET_REQUIRED);
    EXPECT_EQ(driver.frees.load(), 0u);
    EXPECT_EQ(driver.host_unregistrations.load(), 0u);
    EXPECT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_INVALID_STATE);
    EXPECT_EQ(
        initialize(api, 1, 1),
        RTFW_DEVICE_STATUS_INVALID_STATE);

    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(driver.allocations.load(), 1u);
    EXPECT_EQ(driver.frees.load(), 1u);
    EXPECT_EQ(driver.host_registrations.load(), 1u);
    EXPECT_EQ(driver.host_unregistrations.load(), 1u);
    auto health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_SHUTDOWN);
    EXPECT_EQ(health.outstanding, 0u);
}

TEST(CudaBackend, ContextLossCompletesAsLostAndCannotSoftReset) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 0), RTFW_DEVICE_STATUS_OK);
    auto requested = submission(
        1,
        rt::cuda_device_opcode_noop);
    ASSERT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_OK);
    driver.lose_context_on_query.store(
        true,
        std::memory_order_release);
    std::uint64_t count = 0;
    const auto completion = poll_one(api, count);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(completion.status, RTFW_DEVICE_STATUS_LOST);
    EXPECT_EQ(api.reset(api.instance), RTFW_DEVICE_STATUS_LOST);
    auto health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_LOST);
    EXPECT_EQ(health.outstanding, 1u);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(driver.stream_syncs.load(), 1u);
    EXPECT_EQ(initialize(api, 1, 0), RTFW_DEVICE_STATUS_LOST);
}

TEST(CudaBackend, ConcurrentSubmissionsStayWithinFixedCapacity) {
    FakeCudaDriver driver;
    driver.complete_on_record.store(true, std::memory_order_release);
    const std::array<rt::CudaStream, 4> streams{
        0x51u, 0x52u, 0x53u, 0x54u};
    constexpr std::size_t capacity = 16;
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, capacity, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, capacity, 0), RTFW_DEVICE_STATUS_OK);

    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::array<std::thread, 4> producers;
    for (std::size_t producer = 0;
         producer < producers.size();
         ++producer) {
        producers[producer] = std::thread([&, producer] {
            for (std::size_t index = 0; index < 32; ++index) {
                auto requested = submission(
                    1 + producer * 32 + index,
                    rt::cuda_device_opcode_noop);
                const auto status =
                    api.submit(api.instance, &requested);
                if (status == RTFW_DEVICE_STATUS_OK) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                } else if (
                    status == RTFW_DEVICE_STATUS_QUEUE_FULL) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    EXPECT_EQ(accepted.load(), capacity);
    EXPECT_EQ(accepted.load() + rejected.load(), 128u);

    std::array<rtfw_device_completion, capacity> completions{};
    std::uint64_t count = 0;
    EXPECT_EQ(
        api.poll(
            api.instance,
            completions.data(),
            completions.size(),
            &count),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(count, capacity);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, RuntimeGraphAcceptsCandidateBackendAtD0) {
    FakeCudaDriver driver;
    driver.complete_on_record.store(true, std::memory_order_release);
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 4, 1));

    rt::Runtime runtime;
    rt::RuntimeConfig runtime_config{};
    runtime_config.callback_capacity = 2;
    runtime_config.worker_count = 2;
    runtime_config.executor_queue_capacity = 8;
    runtime_config.task_scratch_slots = 8;
    runtime_config.device_backend_capacity = 1;
    runtime_config.device_buffer_capacity = 1;
    runtime_config.device_outstanding_capacity = 4;
    runtime_config.device_completion_batch = 4;
    ASSERT_EQ(
        runtime.configure(runtime_config),
        rt::Status::ok);

    rt::DeviceBackendHandle backend_handle;
    rt::PhaseHandle device_phase;
    rt::PhaseHandle dependent_phase;
    std::atomic<bool> dependent_called{false};
    ASSERT_EQ(
        runtime.register_device_backend(
            {"cuda.fake", backend.api()},
            backend_handle),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_device_phase(
            {
                "cuda.noop",
                backend_handle,
                &prepare_cuda_noop,
                nullptr,
            },
            device_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {
                "cuda.dependent",
                &mark_cuda_dependent,
                &dependent_called,
            },
            dependent_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.add_dependency(device_phase, dependent_phase),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        runtime.step({
            0,
            std::chrono::milliseconds(1),
            std::nullopt,
        }),
        rt::Status::ok);
    EXPECT_TRUE(
        dependent_called.load(std::memory_order_acquire));
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

} // namespace

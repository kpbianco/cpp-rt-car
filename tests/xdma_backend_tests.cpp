#if defined(NDEBUG)
#undef NDEBUG
#endif

#include <rt/xdma_backend.hpp>
#if defined(RTFW_XDMA_LINUX_AVAILABLE)
#include <rt/xdma_linux.hpp>
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#if defined(RTFW_XDMA_LINUX_AVAILABLE)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> track_allocations{false};
std::atomic<std::uint64_t> allocations{0};

struct FakeDriver {
    // Keep the deterministic fake below Windows' default 1 MiB thread stack.
    // The largest stress range is 64 KiB; 128 KiB leaves explicit headroom.
    std::array<std::byte, 1u << 17u> device{};
    std::atomic<bool> initialized{false};
    std::atomic<bool> blocked{false};
    std::atomic<bool> in_transfer{false};
    std::atomic<std::int32_t> next_result{
        static_cast<std::int32_t>(rt::XdmaDriverResult::success)};
    std::atomic<std::uint64_t> now_ns{1};
    std::atomic<std::uint64_t> transfers{0};
    std::atomic<std::uint64_t> resets{0};
    std::atomic<std::uint64_t> shutdowns{0};
    std::array<std::uint32_t, 64> control{};
    std::atomic<std::uint64_t> control_reads{0};
    std::atomic<std::uint64_t> control_writes{0};
    std::atomic<std::uint64_t> event_waits{0};
    std::atomic<std::uint64_t> stop_requests{0};
    std::atomic<bool> event_ready{false};
    std::atomic<bool> event_waiting{false};
    std::atomic<std::uint32_t> event_value{0};
    std::atomic<bool> fail_initialize_after_acquire_once{false};
    std::atomic<bool> fail_shutdown_once{false};

    static FakeDriver* self(void* user_data) noexcept {
        return static_cast<FakeDriver*>(user_data);
    }

    static rt::XdmaDriverResult initialize(void* user_data) noexcept {
        auto* fake = self(user_data);
        bool expected = false;
        if (!fake ||
            !fake->initialized.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return rt::XdmaDriverResult::invalid_value;
        }
        if (fake->fail_initialize_after_acquire_once.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::XdmaDriverResult::io_error;
        }
        return rt::XdmaDriverResult::success;
    }

    static rt::XdmaTransferResult transfer(
        void* user_data,
        rt::XdmaDirection direction,
        std::uint32_t,
        std::uint64_t device_offset,
        void* host_data,
        std::uint64_t bytes) noexcept {
        auto* fake = self(user_data);
        rt::XdmaTransferResult output{};
        if (!fake || !host_data ||
            device_offset > fake->device.size() ||
            bytes > fake->device.size() - device_offset) {
            output.result = rt::XdmaDriverResult::invalid_value;
            return output;
        }
        fake->in_transfer.store(true, std::memory_order_release);
        while (fake->blocked.load(std::memory_order_acquire)) {
            fake->blocked.wait(true, std::memory_order_relaxed);
        }
        const auto requested = static_cast<rt::XdmaDriverResult>(
            fake->next_result.exchange(
                static_cast<std::int32_t>(
                    rt::XdmaDriverResult::success),
                std::memory_order_acq_rel));
        if (requested != rt::XdmaDriverResult::success) {
            output.result = requested;
            fake->in_transfer.store(false, std::memory_order_release);
            return output;
        }
        auto* device = fake->device.data() +
            static_cast<std::size_t>(device_offset);
        if (direction == rt::XdmaDirection::host_to_card) {
            std::memcpy(
                device,
                host_data,
                static_cast<std::size_t>(bytes));
        } else {
            std::memcpy(
                host_data,
                device,
                static_cast<std::size_t>(bytes));
        }
        output.result = rt::XdmaDriverResult::success;
        output.bytes_transferred = bytes;
        fake->transfers.fetch_add(1, std::memory_order_relaxed);
        fake->now_ns.fetch_add(10, std::memory_order_relaxed);
        fake->in_transfer.store(false, std::memory_order_release);
        return output;
    }

    static rt::XdmaDriverResult reset(void* user_data) noexcept {
        auto* fake = self(user_data);
        if (!fake || !fake->initialized.load(std::memory_order_acquire)) {
            return rt::XdmaDriverResult::invalid_value;
        }
        fake->resets.fetch_add(1, std::memory_order_relaxed);
        return rt::XdmaDriverResult::success;
    }

    static rt::XdmaDriverResult shutdown(void* user_data) noexcept {
        auto* fake = self(user_data);
        if (!fake) {
            return rt::XdmaDriverResult::invalid_value;
        }
        fake->shutdowns.fetch_add(1, std::memory_order_relaxed);
        if (fake->fail_shutdown_once.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::XdmaDriverResult::reset_required;
        }
        bool expected = true;
        return fake->initialized.compare_exchange_strong(
                   expected,
                   false,
                   std::memory_order_acq_rel)
            ? rt::XdmaDriverResult::success
            : rt::XdmaDriverResult::invalid_value;
    }

    static std::uint64_t monotonic(void* user_data) noexcept {
        auto* fake = self(user_data);
        return fake ? fake->now_ns.load(std::memory_order_acquire) : 0;
    }

    static rt::XdmaControlReadResult control_read32(
        void* user_data,
        std::uint32_t offset) noexcept {
        auto* fake = self(user_data);
        rt::XdmaControlReadResult output{};
        if (!fake || (offset & 3u) != 0 || offset / 4u >= fake->control.size()) {
            output.result = rt::XdmaDriverResult::invalid_value;
            return output;
        }
        fake->control_reads.fetch_add(1, std::memory_order_relaxed);
        output.value = fake->control[offset / 4u];
        output.result = rt::XdmaDriverResult::success;
        return output;
    }

    static rt::XdmaDriverResult control_write32(
        void* user_data,
        std::uint32_t offset,
        std::uint32_t value) noexcept {
        auto* fake = self(user_data);
        if (!fake || (offset & 3u) != 0 || offset / 4u >= fake->control.size()) {
            return rt::XdmaDriverResult::invalid_value;
        }
        fake->control[offset / 4u] = value;
        fake->control_writes.fetch_add(1, std::memory_order_relaxed);
        return rt::XdmaDriverResult::success;
    }

    static rt::XdmaUserEventResult wait_user_event(
        void* user_data,
        std::uint32_t index,
        std::uint64_t timeout_ns) noexcept {
        auto* fake = self(user_data);
        rt::XdmaUserEventResult output{};
        if (!fake || index >= rt::xdma_user_event_capacity || timeout_ns == 0) {
            output.result = rt::XdmaDriverResult::invalid_value;
            return output;
        }
        fake->event_waits.fetch_add(1, std::memory_order_relaxed);
        fake->event_waiting.store(true, std::memory_order_release);
        while (!fake->event_ready.load(std::memory_order_acquire) &&
               fake->blocked.load(std::memory_order_acquire)) {
            fake->blocked.wait(true, std::memory_order_relaxed);
        }
        fake->event_waiting.store(false, std::memory_order_release);
        if (!fake->event_ready.exchange(false, std::memory_order_acq_rel)) {
            output.result = rt::XdmaDriverResult::timeout;
            return output;
        }
        output.result = rt::XdmaDriverResult::success;
        output.value = fake->event_value.load(std::memory_order_acquire);
        return output;
    }

    static rt::XdmaDriverResult request_stop(void* user_data) noexcept {
        auto* fake = self(user_data);
        if (!fake) {
            return rt::XdmaDriverResult::invalid_value;
        }
        fake->stop_requests.fetch_add(1, std::memory_order_relaxed);
        fake->blocked.store(false, std::memory_order_release);
        fake->blocked.notify_all();
        return rt::XdmaDriverResult::success;
    }

    rt::XdmaDriverApi api() noexcept {
        rt::XdmaDriverApi output{};
        output.user_data = this;
        output.initialize = &initialize;
        output.transfer = &transfer;
        output.reset = &reset;
        output.shutdown = &shutdown;
        output.monotonic_time_ns = &monotonic;
        return output;
    }

    rt::XdmaDriverApi api_v2() noexcept {
        auto output = api();
        output.struct_size = sizeof(output);
        output.api_version = rt::xdma_driver_api_version_2;
        output.control_read32 = &control_read32;
        output.control_write32 = &control_write32;
        output.wait_user_event = &wait_user_event;
        output.request_stop = &request_stop;
        return output;
    }
};

rtfw_device_backend_api initialize_backend(
    rt::XdmaDeviceBackend& backend,
    std::uint64_t in_flight,
    std::uint64_t buffers) {
    auto api = backend.api();
    rtfw_device_init_config config{};
    config.struct_size = sizeof(config);
    config.abi_version = RTFW_DEVICE_ABI_VERSION;
    config.requested_in_flight = in_flight;
    config.requested_registered_buffers = buffers;
    assert(api.initialize(api.instance, &config) == RTFW_DEVICE_STATUS_OK);
    return api;
}

std::uint64_t register_buffer(
    rtfw_device_backend_api& api,
    std::span<std::byte> storage) {
    rtfw_device_buffer_registration registration{};
    registration.struct_size = sizeof(registration);
    registration.flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    registration.data = storage.data();
    registration.bytes = storage.size();
    std::memcpy(registration.name, "test.buffer", sizeof("test.buffer"));
    std::uint64_t token = 0;
    assert(
        api.register_buffer(
            api.instance,
            &registration,
            &token) == RTFW_DEVICE_STATUS_OK);
    return token;
}

rtfw_device_submission submission(
    std::uint64_t id,
    std::uint64_t token,
    rt::XdmaDirection direction,
    std::uint64_t device_offset,
    std::uint64_t bytes,
    std::uint64_t timeout_ns = 1'000'000'000) {
    rtfw_device_submission output{};
    output.struct_size = sizeof(output);
    output.abi_version = RTFW_DEVICE_ABI_VERSION;
    output.submission_id = id;
    output.timeout_ns = timeout_ns;
    output.buffer_count = 1;
    output.buffers[0].buffer_token = token;
    output.buffers[0].access =
        direction == rt::XdmaDirection::host_to_card
        ? RTFW_DEVICE_ACCESS_READ
        : RTFW_DEVICE_ACCESS_WRITE;
    output.buffers[0].bytes = bytes;
    rt::XdmaTransfer transfer{};
    transfer.device_offset = device_offset;
    rt::set_xdma_transfer(output, direction, transfer);
    return output;
}

rtfw_device_completion wait_for(
    rtfw_device_backend_api& api,
    std::uint64_t id) {
    for (;;) {
        rtfw_device_completion completion{};
        std::uint64_t count = 0;
        assert(
            api.poll(api.instance, &completion, 1, &count) ==
            RTFW_DEVICE_STATUS_OK);
        if (count != 0) {
            assert(completion.submission_id == id);
            return completion;
        }
        std::this_thread::yield();
    }
}

struct NativeFixture {
    rt::HalV2BackendRegistration registration{};
    rt::HalV2BackendApi core{};
    rt::HalV2CommandTimelineExtension command{};
    rt::HalV2MemoryTopologyExtension memory{};
};

NativeFixture initialize_native(rt::XdmaDeviceBackend& backend) {
    NativeFixture fixture{};
    fixture.registration = backend.hal_v2_registration("test.xdma.native");
    fixture.core = fixture.registration.api;
    fixture.command = *fixture.registration.command_timeline;
    fixture.memory = *fixture.registration.memory_topology;
    rt::HalV2InitializeConfig config{};
    config.requested_in_flight = 4;
    config.requested_registered_buffers = 2;
    assert(fixture.core.initialize(fixture.core.instance, &config) ==
           rt::HalV2Status::ok);
    return fixture;
}

std::uint64_t register_native_buffer(
    NativeFixture& fixture,
    std::span<std::byte> storage) {
    rt::HalV2BufferRegistration registration{};
    registration.flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    registration.data = storage.data();
    registration.bytes = storage.size();
    std::memcpy(registration.name.data(), "native.buffer", 14);
    std::uint64_t token = 0;
    assert(fixture.core.register_buffer(
               fixture.core.instance, &registration, &token) ==
           rt::HalV2Status::ok);
    return token;
}

rt::DeviceCommandBatch native_batch(std::uint64_t id) {
    rt::DeviceCommandBatch batch{};
    batch.batch_id = id;
    batch.timeout_ns = 1'000'000;
    batch.signal_count = 1;
    batch.signals[0].timeline_handle = 1;
    batch.signals[0].value = id;
    return batch;
}

rt::HalV2BatchCompletion wait_for_batch(
    NativeFixture& fixture,
    std::uint64_t id) {
    for (;;) {
        rt::HalV2BatchCompletion completion{};
        std::uint64_t count = 0;
        assert(fixture.command.poll(
                   fixture.command.instance, &completion, 1, &count) ==
               rt::HalV2Status::ok);
        if (count != 0) {
            assert(completion.batch_id == id);
            return completion;
        }
        std::this_thread::yield();
    }
}

void basic_round_trip() {
    FakeDriver fake;
    rt::XdmaBackendConfig config{};
    config.queue_capacity = 4;
    config.buffer_capacity = 2;
    config.worker_count = 2;
    config.max_transfer_bytes = 4096;
    rt::XdmaDeviceBackend backend(fake.api(), config);
    auto api = initialize_backend(backend, 4, 1);
    std::array<std::byte, 4096> storage{};
    for (std::size_t index = 0; index < storage.size(); ++index) {
        storage[index] = static_cast<std::byte>(index & 0xffu);
    }
    const auto expected = storage;
    const auto token = register_buffer(api, storage);
    auto upload = submission(
        1,
        token,
        rt::XdmaDirection::host_to_card,
        4096,
        storage.size());
    assert(api.submit(api.instance, &upload) == RTFW_DEVICE_STATUS_OK);
    assert(wait_for(api, 1).status == RTFW_DEVICE_STATUS_OK);
    storage.fill(std::byte{0});
    auto download = submission(
        2,
        token,
        rt::XdmaDirection::card_to_host,
        4096,
        storage.size());
    assert(api.submit(api.instance, &download) == RTFW_DEVICE_STATUS_OK);
    assert(wait_for(api, 2).status == RTFW_DEVICE_STATUS_OK);
    assert(storage == expected);
    assert(api.unregister_buffer(api.instance, token) == RTFW_DEVICE_STATUS_OK);
    assert(api.shutdown(api.instance) == RTFW_DEVICE_STATUS_OK);
}

void saturation_and_timeout_quarantine() {
    FakeDriver fake;
    fake.blocked.store(true, std::memory_order_release);
    rt::XdmaBackendConfig config{};
    config.queue_capacity = 2;
    config.buffer_capacity = 1;
    config.worker_count = 1;
    config.max_transfer_bytes = 4096;
    rt::XdmaDeviceBackend backend(fake.api(), config);
    auto api = initialize_backend(backend, 2, 1);
    std::array<std::byte, 4096> storage{};
    const auto token = register_buffer(api, storage);
    auto first = submission(
        11,
        token,
        rt::XdmaDirection::host_to_card,
        0,
        storage.size(),
        50);
    auto second = submission(
        12,
        token,
        rt::XdmaDirection::host_to_card,
        4096,
        storage.size());
    auto third = submission(
        13,
        token,
        rt::XdmaDirection::host_to_card,
        8192,
        storage.size());
    assert(api.submit(api.instance, &first) == RTFW_DEVICE_STATUS_OK);
    while (!fake.in_transfer.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    assert(api.submit(api.instance, &second) == RTFW_DEVICE_STATUS_OK);
    assert(api.submit(api.instance, &third) == RTFW_DEVICE_STATUS_QUEUE_FULL);
    fake.now_ns.store(100, std::memory_order_release);
    rtfw_device_completion completion{};
    std::uint64_t count = 0;
    assert(
        api.poll(api.instance, &completion, 1, &count) ==
        RTFW_DEVICE_STATUS_OK);
    assert(count == 0);
    fake.blocked.store(false, std::memory_order_release);
    fake.blocked.notify_all();
    assert(wait_for(api, 11).status == RTFW_DEVICE_STATUS_TIMEOUT);
    assert(wait_for(api, 12).status == RTFW_DEVICE_STATUS_OK);
    assert(api.shutdown(api.instance) == RTFW_DEVICE_STATUS_OK);
}

void validation_rejects_malformed_work() {
    FakeDriver fake;
    bool threw = false;
    try {
        rt::XdmaBackendConfig invalid{};
        invalid.queue_capacity = 0;
        rt::XdmaDeviceBackend rejected(fake.api(), invalid);
        (void)rejected;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    rt::XdmaBackendConfig config{};
    config.queue_capacity = 2;
    config.buffer_capacity = 1;
    config.worker_count = 1;
    config.max_transfer_bytes = 4096;
    config.max_buffer_bytes = 4096;
    config.transfer_alignment = 64;
    rt::XdmaDeviceBackend backend(fake.api(), config);
    auto api = initialize_backend(backend, 2, 1);

    std::array<std::byte, 4097> unaligned{};
    rtfw_device_buffer_registration bad_registration{};
    bad_registration.struct_size = sizeof(bad_registration);
    bad_registration.flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    auto* misaligned = unaligned.data();
    if ((reinterpret_cast<std::uintptr_t>(misaligned) & 63u) == 0) {
        ++misaligned;
    }
    bad_registration.data = misaligned;
    bad_registration.bytes = 4096;
    std::memcpy(
        bad_registration.name,
        "bad.alignment",
        sizeof("bad.alignment"));
    std::uint64_t token = 0;
    assert(
        api.register_buffer(
            api.instance,
            &bad_registration,
            &token) == RTFW_DEVICE_STATUS_INVALID_ARGUMENT);

    alignas(64) std::array<std::byte, 4096> storage{};
    token = register_buffer(api, storage);
    auto work = submission(
        31,
        token,
        rt::XdmaDirection::host_to_card,
        0,
        storage.size());
    rt::XdmaTransfer invalid_transfer{};
    invalid_transfer.channel = 1;
    rt::set_xdma_transfer(
        work,
        rt::XdmaDirection::host_to_card,
        invalid_transfer);
    assert(
        api.submit(api.instance, &work) ==
        RTFW_DEVICE_STATUS_INVALID_ARGUMENT);
    work = submission(
        32,
        token,
        rt::XdmaDirection::host_to_card,
        1,
        storage.size());
    assert(
        api.submit(api.instance, &work) ==
        RTFW_DEVICE_STATUS_INVALID_ARGUMENT);
    work = submission(
        33,
        token,
        rt::XdmaDirection::host_to_card,
        0,
        storage.size());
    work.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
    assert(
        api.submit(api.instance, &work) ==
        RTFW_DEVICE_STATUS_INVALID_ARGUMENT);
    assert(api.shutdown(api.instance) == RTFW_DEVICE_STATUS_OK);
}

void recovery_and_no_allocation() {
    FakeDriver fake;
    rt::XdmaBackendConfig config{};
    config.queue_capacity = 4;
    config.buffer_capacity = 1;
    config.worker_count = 1;
    config.max_transfer_bytes = 4096;
    rt::XdmaDeviceBackend backend(fake.api(), config);
    auto api = initialize_backend(backend, 4, 1);
    std::array<std::byte, 4096> storage{};
    const auto token = register_buffer(api, storage);

    fake.next_result.store(
        static_cast<std::int32_t>(
            rt::XdmaDriverResult::reset_required),
        std::memory_order_release);
    auto failed = submission(
        21,
        token,
        rt::XdmaDirection::host_to_card,
        0,
        storage.size());
    assert(api.submit(api.instance, &failed) == RTFW_DEVICE_STATUS_OK);
    assert(wait_for(api, 21).status == RTFW_DEVICE_STATUS_RESET_REQUIRED);
    rtfw_device_health health{};
    health.struct_size = sizeof(health);
    assert(
        api.get_health(api.instance, &health) ==
        RTFW_DEVICE_STATUS_OK);
    assert(health.state == RTFW_DEVICE_HEALTH_RESET_REQUIRED);
    assert(api.reset(api.instance) == RTFW_DEVICE_STATUS_OK);

    allocations.store(0, std::memory_order_release);
    track_allocations.store(true, std::memory_order_release);
    auto transfer = submission(
        22,
        token,
        rt::XdmaDirection::host_to_card,
        0,
        storage.size());
    assert(api.submit(api.instance, &transfer) == RTFW_DEVICE_STATUS_OK);
    assert(wait_for(api, 22).status == RTFW_DEVICE_STATUS_OK);
    track_allocations.store(false, std::memory_order_release);
    assert(allocations.load(std::memory_order_acquire) == 0);
    fake.fail_shutdown_once.store(true, std::memory_order_release);
    assert(
        api.shutdown(api.instance) ==
        RTFW_DEVICE_STATUS_RESET_REQUIRED);
    assert(api.shutdown(api.instance) == RTFW_DEVICE_STATUS_OK);
}

void partial_initialize_cleanup_retries() {
    FakeDriver fake;
    fake.fail_initialize_after_acquire_once.store(
        true,
        std::memory_order_release);
    fake.fail_shutdown_once.store(true, std::memory_order_release);
    rt::XdmaBackendConfig config{};
    config.queue_capacity = 2;
    config.buffer_capacity = 1;
    config.worker_count = 1;
    rt::XdmaDeviceBackend backend(fake.api(), config);
    auto api = backend.api();
    rtfw_device_init_config requested{};
    requested.struct_size = sizeof(requested);
    requested.abi_version = RTFW_DEVICE_ABI_VERSION;
    requested.requested_in_flight = 2;
    requested.requested_registered_buffers = 1;

    assert(
        api.initialize(api.instance, &requested) ==
        RTFW_DEVICE_STATUS_ERROR);
    assert(fake.initialized.load(std::memory_order_acquire));
    assert(fake.shutdowns.load(std::memory_order_acquire) == 1);
    assert(api.shutdown(api.instance) == RTFW_DEVICE_STATUS_OK);
    assert(!fake.initialized.load(std::memory_order_acquire));
    assert(fake.shutdowns.load(std::memory_order_acquire) == 2);
    assert(
        api.shutdown(api.instance) ==
        RTFW_DEVICE_STATUS_INVALID_STATE);
    assert(fake.shutdowns.load(std::memory_order_acquire) == 2);
}

void concurrent_submit_poll() {
    FakeDriver fake;
    rt::XdmaBackendConfig config{};
    config.queue_capacity = 64;
    config.buffer_capacity = 1;
    config.worker_count = 4;
    config.max_transfer_bytes = 1u << 16u;
    rt::XdmaDeviceBackend backend(fake.api(), config);
    auto api = initialize_backend(backend, 64, 1);
    std::array<std::byte, 1u << 16u> storage{};
    const auto token = register_buffer(api, storage);

    constexpr std::uint64_t producer_count = 4;
    constexpr std::uint64_t per_producer = 250;
    std::atomic<std::uint64_t> producers_done{0};
    std::array<std::thread, producer_count> producers;
    for (std::uint64_t producer = 0;
         producer < producer_count;
         ++producer) {
        producers[producer] = std::thread([&, producer] {
            for (std::uint64_t index = 0; index < per_producer; ++index) {
                const auto id = producer * per_producer + index + 1;
                auto work = submission(
                    id,
                    token,
                    rt::XdmaDirection::host_to_card,
                    id * 64,
                    64);
                while (api.submit(api.instance, &work) ==
                       RTFW_DEVICE_STATUS_QUEUE_FULL) {
                    std::this_thread::yield();
                }
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    std::uint64_t completed = 0;
    while (completed < producer_count * per_producer) {
        std::array<rtfw_device_completion, 32> batch{};
        std::uint64_t count = 0;
        assert(
            api.poll(
                api.instance,
                batch.data(),
                batch.size(),
                &count) == RTFW_DEVICE_STATUS_OK);
        for (std::uint64_t index = 0; index < count; ++index) {
            assert(batch[index].status == RTFW_DEVICE_STATUS_OK);
            assert(batch[index].value == 64);
        }
        completed += count;
        if (count == 0) {
            std::this_thread::yield();
        }
    }
    for (auto& producer : producers) {
        producer.join();
    }
    assert(
        producers_done.load(std::memory_order_acquire) ==
        producer_count);
    assert(api.shutdown(api.instance) == RTFW_DEVICE_STATUS_OK);
}

void driver_versions_and_native_snapshot() {
    FakeDriver fake;
    const auto v1 = fake.api();
    assert(v1.struct_size == rt::xdma_driver_api_v1_size);
    assert(v1.api_version == rt::xdma_driver_api_version_1);
    assert(v1.control_read32 == nullptr);

    rt::XdmaBackendConfig config{};
    config.queue_capacity = 4;
    config.buffer_capacity = 2;
    config.worker_count = 1;
    config.max_transfer_bytes = 4096;
    config.control_aperture_bytes = 256;
    config.user_event_count = 2;
    rt::XdmaDeviceBackend backend(fake.api_v2(), config);
    auto registration = backend.hal_v2_registration("test.xdma.native");
    assert(registration.api.api_version == rt::hal_v2_api_version);
    assert(registration.memory_topology != nullptr);
    assert(registration.command_timeline != nullptr);

    rt::HalV2Capabilities capabilities{};
    assert(registration.api.get_capabilities(
               registration.api.instance, &capabilities) ==
           rt::HalV2Status::ok);
    assert(std::string_view(capabilities.backend_id.data()) ==
           "rtfw.xdma.xilinx_linux_aximm.v2");
    rt::HalV2MemoryTopologySnapshot snapshot{};
    assert(registration.memory_topology->discover(
               registration.memory_topology->instance, &snapshot) ==
           rt::HalV2Status::ok);
    assert(snapshot.memory_domain_count == 1);
    assert(snapshot.topology_nodes[0].kind == static_cast<std::uint32_t>(
               rt::HalV2TopologyNodeKind::dma_endpoint));
    assert(snapshot.completion_timestamp_domain_identity == 1);

    auto old_api = backend.api();
    auto native = initialize_native(backend);
    rtfw_device_init_config old_initialize{};
    old_initialize.struct_size = sizeof(old_initialize);
    old_initialize.abi_version = RTFW_DEVICE_ABI_VERSION;
    old_initialize.requested_in_flight = 1;
    old_initialize.requested_registered_buffers = 1;
    assert(old_api.initialize(old_api.instance, &old_initialize) ==
           RTFW_DEVICE_STATUS_INVALID_STATE);
    assert(native.core.shutdown(native.core.instance) == rt::HalV2Status::ok);

    bool threw = false;
    try {
        auto invalid_driver = fake.api_v2();
        invalid_driver.reserved_v2[0] = 1;
        rt::XdmaDeviceBackend invalid(invalid_driver, config);
        (void)invalid;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    threw = false;
    try {
        auto invalid_config = config;
        invalid_config.control_aperture_bytes = 258;
        rt::XdmaDeviceBackend invalid(fake.api_v2(), invalid_config);
        (void)invalid;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
}

void native_control_event_and_malformed_validation() {
    FakeDriver fake;
    fake.control[3] = 0x1122'3344u;
    fake.event_ready.store(true, std::memory_order_release);
    fake.event_value.store(0xaabb'ccddu, std::memory_order_release);
    rt::XdmaBackendConfig config{};
    config.queue_capacity = 4;
    config.buffer_capacity = 2;
    config.worker_count = 1;
    config.max_transfer_bytes = 4096;
    config.control_aperture_bytes = 256;
    config.user_event_count = 2;
    rt::XdmaDeviceBackend backend(fake.api_v2(), config);
    auto fixture = initialize_native(backend);
    std::array<std::byte, 16> output{};
    const auto token = register_native_buffer(fixture, output);
    rt::HalV2BufferReference reference{};
    reference.buffer_token = token;
    reference.access = RTFW_DEVICE_ACCESS_WRITE;
    reference.bytes = 4;

    auto batch = native_batch(101);
    batch.command_count = 3;
    assert(rt::set_xdma_control_write(
        batch.commands[0], 8, 0x5566'7788u));
    assert(rt::set_xdma_control_read(batch.commands[1], 12, reference));
    reference.offset = 4;
    assert(rt::set_xdma_user_event_wait(batch.commands[2], 1, reference));
    assert(fixture.command.submit(fixture.command.instance, &batch) ==
           rt::HalV2Status::ok);
    const auto completion = wait_for_batch(fixture, 101);
    assert(completion.status == static_cast<std::int32_t>(rt::HalV2Status::ok));
    assert(fake.control[2] == 0x5566'7788u);
    assert(fake.control_reads.load(std::memory_order_acquire) == 1);
    assert(fake.control_writes.load(std::memory_order_acquire) == 1);
    assert(fake.event_waits.load(std::memory_order_acquire) == 1);
    std::uint32_t read_value = 0;
    std::uint32_t event_value = 0;
    std::memcpy(&read_value, output.data(), sizeof(read_value));
    std::memcpy(&event_value, output.data() + 4, sizeof(event_value));
    assert(read_value == 0x1122'3344u);
    assert(event_value == 0xaabb'ccddu);

    const auto calls = fake.control_reads.load(std::memory_order_acquire);
    auto malformed = native_batch(102);
    malformed.command_count = 1;
    reference.offset = 0;
    assert(rt::set_xdma_control_read(malformed.commands[0], 12, reference));
    malformed.commands[0].buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    assert(fixture.command.submit(fixture.command.instance, &malformed) ==
           rt::HalV2Status::invalid_argument);
    malformed = native_batch(103);
    malformed.command_count = 1;
    assert(rt::set_xdma_control_read(malformed.commands[0], 12, reference));
    malformed.commands[0].payload[127] = 1;
    assert(fixture.command.submit(fixture.command.instance, &malformed) ==
           rt::HalV2Status::invalid_argument);
    malformed = native_batch(104);
    malformed.command_count = 1;
    malformed.commands[0].kind = static_cast<std::uint32_t>(
        rt::HalV2CommandKind::dispatch);
    malformed.commands[0].opcode = 0x584b'0000u;
    assert(fixture.command.submit(fixture.command.instance, &malformed) ==
           rt::HalV2Status::invalid_argument);
    malformed = native_batch(105);
    malformed.command_count = 1;
    assert(rt::set_xdma_control_read(malformed.commands[0], 252, reference));
    assert(fixture.command.submit(fixture.command.instance, &malformed) ==
           rt::HalV2Status::ok);
    assert(wait_for_batch(fixture, 105).status ==
           static_cast<std::int32_t>(rt::HalV2Status::ok));
    malformed = native_batch(106);
    malformed.command_count = 1;
    malformed.commands[0].kind = static_cast<std::uint32_t>(
        rt::HalV2CommandKind::dispatch);
    malformed.commands[0].opcode =
        rt::xdma_device_opcode_control_read_base | 64u;
    malformed.commands[0].buffer_count = 1;
    malformed.commands[0].buffers[0] = reference;
    assert(fixture.command.submit(fixture.command.instance, &malformed) ==
           rt::HalV2Status::invalid_argument);
    assert(fake.control_reads.load(std::memory_order_acquire) == calls + 1);

    assert(fixture.core.unregister_buffer(fixture.core.instance, token) ==
           rt::HalV2Status::ok);
    assert(fixture.core.shutdown(fixture.core.instance) == rt::HalV2Status::ok);
}

void native_timeout_stop_and_isolation() {
    FakeDriver first;
    FakeDriver second;
    rt::XdmaBackendConfig config{};
    config.queue_capacity = 4;
    config.buffer_capacity = 2;
    config.worker_count = 1;
    config.max_transfer_bytes = 4096;
    config.user_event_count = 1;
    rt::XdmaDeviceBackend first_backend(first.api_v2(), config);
    rt::XdmaDeviceBackend second_backend(second.api_v2(), config);
    auto first_fixture = initialize_native(first_backend);
    auto second_fixture = initialize_native(second_backend);
    std::array<std::byte, 4> first_output{};
    std::array<std::byte, 4> second_output{};
    const auto first_token = register_native_buffer(first_fixture, first_output);
    const auto second_token = register_native_buffer(second_fixture, second_output);

    rt::HalV2BufferReference reference{};
    reference.buffer_token = first_token;
    reference.access = RTFW_DEVICE_ACCESS_WRITE;
    reference.bytes = 4;
    auto timeout = native_batch(201);
    timeout.command_count = 1;
    assert(rt::set_xdma_user_event_wait(timeout.commands[0], 0, reference));
    assert(first_fixture.command.submit(
               first_fixture.command.instance, &timeout) ==
           rt::HalV2Status::ok);
    assert(wait_for_batch(first_fixture, 201).status ==
           static_cast<std::int32_t>(rt::HalV2Status::timeout));
    assert(second.event_waits.load(std::memory_order_acquire) == 0);

    first.blocked.store(true, std::memory_order_release);
    auto blocked = native_batch(202);
    blocked.command_count = 1;
    assert(rt::set_xdma_user_event_wait(blocked.commands[0], 0, reference));
    assert(first_fixture.command.submit(
               first_fixture.command.instance, &blocked) ==
           rt::HalV2Status::ok);
    while (!first.event_waiting.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    assert(first_fixture.command.request_stop(
               first_fixture.command.instance) == rt::HalV2Status::ok);
    assert(first_fixture.command.request_stop(
               first_fixture.command.instance) == rt::HalV2Status::ok);
    assert(wait_for_batch(first_fixture, 202).status ==
           static_cast<std::int32_t>(rt::HalV2Status::canceled));
    assert(first.stop_requests.load(std::memory_order_acquire) >= 1);
    assert(second.stop_requests.load(std::memory_order_acquire) == 0);

    assert(first_fixture.core.unregister_buffer(
               first_fixture.core.instance, first_token) ==
           rt::HalV2Status::ok);
    assert(second_fixture.core.unregister_buffer(
               second_fixture.core.instance, second_token) ==
           rt::HalV2Status::ok);
    assert(first_fixture.core.shutdown(first_fixture.core.instance) ==
           rt::HalV2Status::ok);
    assert(second_fixture.core.shutdown(second_fixture.core.instance) ==
           rt::HalV2Status::ok);
}

void native_queued_cancel_does_not_interrupt_running_event() {
    FakeDriver fake;
    fake.blocked.store(true, std::memory_order_release);
    rt::XdmaBackendConfig config{};
    config.queue_capacity = 4;
    config.buffer_capacity = 2;
    config.worker_count = 1;
    config.max_transfer_bytes = 4096;
    config.control_aperture_bytes = 256;
    config.user_event_count = 1;
    rt::XdmaDeviceBackend backend(fake.api_v2(), config);
    auto fixture = initialize_native(backend);
    std::array<std::byte, 4> output{};
    const auto token = register_native_buffer(fixture, output);

    rt::HalV2BufferReference reference{};
    reference.buffer_token = token;
    reference.access = RTFW_DEVICE_ACCESS_WRITE;
    reference.bytes = sizeof(std::uint32_t);
    auto running = native_batch(301);
    running.command_count = 1;
    assert(rt::set_xdma_user_event_wait(
        running.commands[0], 0, reference));
    assert(fixture.command.submit(fixture.command.instance, &running) ==
           rt::HalV2Status::ok);
    while (!fake.event_waiting.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto queued = native_batch(302);
    queued.command_count = 1;
    assert(rt::set_xdma_control_write(
        queued.commands[0], 0, 0x1234'5678u));
    assert(fixture.command.submit(fixture.command.instance, &queued) ==
           rt::HalV2Status::ok);
    assert(fixture.command.cancel(fixture.command.instance, 302) ==
           rt::HalV2Status::ok);
    assert(fake.stop_requests.load(std::memory_order_acquire) == 0);
    assert(fake.event_waiting.load(std::memory_order_acquire));
    assert(fake.control_writes.load(std::memory_order_acquire) == 0);

    rt::HalV2BatchCompletion canceled{};
    std::uint64_t count = 0;
    assert(fixture.command.poll(
               fixture.command.instance, &canceled, 1, &count) ==
           rt::HalV2Status::ok);
    assert(count == 1);
    assert(canceled.batch_id == 302);
    assert(canceled.status ==
           static_cast<std::int32_t>(rt::HalV2Status::canceled));

    fake.event_value.store(0xaabb'ccddu, std::memory_order_release);
    fake.event_ready.store(true, std::memory_order_release);
    fake.blocked.store(false, std::memory_order_release);
    fake.blocked.notify_all();
    const auto completed = wait_for_batch(fixture, 301);
    assert(completed.status ==
           static_cast<std::int32_t>(rt::HalV2Status::ok));

    rt::HalV2Health health{};
    assert(fixture.core.get_health(fixture.core.instance, &health) ==
           rt::HalV2Status::ok);
    assert(health.submissions == 2);
    assert(health.completions == 2);
    assert(health.cancellations == 1);
    assert(health.outstanding == 0);
    assert(health.state ==
           static_cast<std::uint32_t>(rt::HalV2HealthState::healthy));

    assert(fixture.core.unregister_buffer(fixture.core.instance, token) ==
           rt::HalV2Status::ok);
    assert(fixture.core.shutdown(fixture.core.instance) ==
           rt::HalV2Status::ok);
}

#if defined(RTFW_XDMA_LINUX_AVAILABLE)
void linux_adapter_smoke() {
    char path[] = "/tmp/rtfw-xdma-XXXXXX";
    const int descriptor = ::mkstemp(path);
    assert(descriptor >= 0);
    assert(::ftruncate(descriptor, 8192) == 0);
    assert(::close(descriptor) == 0);

    const std::array<std::string_view, 1> h2c{path};
    const std::array<std::string_view, 1> c2h{path};
    rt::LinuxXdmaConfig driver_config{};
    driver_config.h2c_paths = h2c;
    driver_config.c2h_paths = c2h;
    rt::LinuxXdmaDriver driver(driver_config);
    auto api = driver.api();
    assert(api.initialize(api.user_data) == rt::XdmaDriverResult::success);
    std::array<std::byte, 4096> storage{};
    storage.fill(std::byte{0x5a});
    auto upload = api.transfer(
        api.user_data,
        rt::XdmaDirection::host_to_card,
        0,
        0,
        storage.data(),
        storage.size());
    assert(upload.result == rt::XdmaDriverResult::success);
    storage.fill(std::byte{0});
    auto download = api.transfer(
        api.user_data,
        rt::XdmaDirection::card_to_host,
        0,
        0,
        storage.data(),
        storage.size());
    assert(download.result == rt::XdmaDriverResult::success);
    for (const auto value : storage) {
        assert(value == std::byte{0x5a});
    }
    assert(api.shutdown(api.user_data) == rt::XdmaDriverResult::success);

    char user_path[] = "/tmp/rtfw-xdma-user-XXXXXX";
    const int user_descriptor = ::mkstemp(user_path);
    assert(user_descriptor >= 0);
    assert(::ftruncate(user_descriptor, 256) == 0);
    assert(::close(user_descriptor) == 0);
    char event_path[] = "/tmp/rtfw-xdma-event-XXXXXX";
    const int event_descriptor = ::mkstemp(event_path);
    assert(event_descriptor >= 0);
    const std::array<std::uint8_t, 8> event_bytes{
        0x78, 0x56, 0x34, 0x12,
        0x78, 0x56, 0x34, 0x12};
    assert(::write(
               event_descriptor, event_bytes.data(), event_bytes.size()) ==
           static_cast<ssize_t>(event_bytes.size()));
    assert(::lseek(event_descriptor, 0, SEEK_SET) == 0);
    assert(::close(event_descriptor) == 0);
    const std::array<std::string_view, 1> events{event_path};
    rt::LinuxXdmaConfig control_only_config{};
    control_only_config.h2c_paths = h2c;
    control_only_config.c2h_paths = c2h;
    control_only_config.user_path = user_path;
    rt::LinuxXdmaDriver control_only_driver(control_only_config);
    const auto control_only_api = control_only_driver.api();
    assert(control_only_api.api_version == rt::xdma_driver_api_version_2);
    assert(control_only_api.control_read32 != nullptr);
    assert(control_only_api.control_write32 != nullptr);
    assert(control_only_api.wait_user_event == nullptr);
    assert(control_only_api.request_stop == nullptr);

    rt::LinuxXdmaConfig event_only_config{};
    event_only_config.h2c_paths = h2c;
    event_only_config.c2h_paths = c2h;
    event_only_config.event_paths = events;
    rt::LinuxXdmaDriver event_only_driver(event_only_config);
    const auto event_only_api = event_only_driver.api();
    assert(event_only_api.api_version == rt::xdma_driver_api_version_2);
    assert(event_only_api.control_read32 == nullptr);
    assert(event_only_api.control_write32 == nullptr);
    assert(event_only_api.wait_user_event != nullptr);
    assert(event_only_api.request_stop != nullptr);

    bool rejected_mismatched_capability = false;
    try {
        rt::XdmaBackendConfig mismatched{};
        mismatched.user_event_count = 1;
        rt::XdmaDeviceBackend invalid(control_only_api, mismatched);
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected_mismatched_capability = true;
    }
    assert(rejected_mismatched_capability);
    rejected_mismatched_capability = false;
    try {
        rt::XdmaBackendConfig mismatched{};
        mismatched.control_aperture_bytes = 256;
        rt::XdmaDeviceBackend invalid(event_only_api, mismatched);
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected_mismatched_capability = true;
    }
    assert(rejected_mismatched_capability);

    rt::LinuxXdmaConfig control_config{};
    control_config.h2c_paths = h2c;
    control_config.c2h_paths = c2h;
    control_config.user_path = user_path;
    control_config.event_paths = events;
    rt::LinuxXdmaDriver control_driver(control_config);
    auto control_api = control_driver.api();
    assert(control_api.api_version == rt::xdma_driver_api_version_2);
    assert(control_api.initialize(control_api.user_data) ==
           rt::XdmaDriverResult::success);
    assert(control_api.control_write32(
               control_api.user_data, 12, 0xaabb'ccddu) ==
           rt::XdmaDriverResult::success);
    const auto read = control_api.control_read32(control_api.user_data, 12);
    assert(read.result == rt::XdmaDriverResult::success);
    assert(read.value == 0xaabb'ccddu);
    const auto event = control_api.wait_user_event(
        control_api.user_data, 0, 1'000'000);
    assert(event.result == rt::XdmaDriverResult::success);
    assert(event.value == 0x1234'5678u);
    assert(control_api.request_stop(control_api.user_data) ==
           rt::XdmaDriverResult::success);
    assert(control_api.request_stop(control_api.user_data) ==
           rt::XdmaDriverResult::success);
    const auto stopped_event = control_api.wait_user_event(
        control_api.user_data, 0, 1'000'000);
    assert(stopped_event.result == rt::XdmaDriverResult::error);
    const auto rearmed_event = control_api.wait_user_event(
        control_api.user_data, 0, 1'000'000);
    assert(rearmed_event.result == rt::XdmaDriverResult::success);
    assert(rearmed_event.value == 0x1234'5678u);
    assert(control_api.shutdown(control_api.user_data) ==
           rt::XdmaDriverResult::success);
    assert(::unlink(path) == 0);
    assert(::unlink(user_path) == 0);
    assert(::unlink(event_path) == 0);
}
#endif

} // namespace

void* operator new(std::size_t bytes) {
    if (track_allocations.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* storage = std::malloc(bytes)) {
        return storage;
    }
    throw std::bad_alloc();
}

void operator delete(void* storage) noexcept {
    std::free(storage);
}

void operator delete(void* storage, std::size_t) noexcept {
    std::free(storage);
}

int main() {
    basic_round_trip();
    saturation_and_timeout_quarantine();
    validation_rejects_malformed_work();
    recovery_and_no_allocation();
    partial_initialize_cleanup_retries();
    concurrent_submit_poll();
    driver_versions_and_native_snapshot();
    native_control_event_and_malformed_validation();
    native_timeout_stop_and_isolation();
    native_queued_cancel_does_not_interrupt_running_event();
#if defined(RTFW_XDMA_LINUX_AVAILABLE)
    linux_adapter_smoke();
#endif
    return 0;
}

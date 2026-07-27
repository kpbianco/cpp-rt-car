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
    std::array<std::byte, 1u << 20u> device{};
    std::atomic<bool> initialized{false};
    std::atomic<bool> blocked{false};
    std::atomic<bool> in_transfer{false};
    std::atomic<std::int32_t> next_result{
        static_cast<std::int32_t>(rt::XdmaDriverResult::success)};
    std::atomic<std::uint64_t> now_ns{1};
    std::atomic<std::uint64_t> transfers{0};
    std::atomic<std::uint64_t> resets{0};
    std::atomic<bool> fail_shutdown_once{false};

    static FakeDriver* self(void* user_data) noexcept {
        return static_cast<FakeDriver*>(user_data);
    }

    static rt::XdmaDriverResult initialize(void* user_data) noexcept {
        auto* fake = self(user_data);
        bool expected = false;
        return fake && fake->initialized.compare_exchange_strong(
                           expected,
                           true,
                           std::memory_order_acq_rel)
            ? rt::XdmaDriverResult::success
            : rt::XdmaDriverResult::invalid_value;
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
        if (fake &&
            fake->fail_shutdown_once.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::XdmaDriverResult::reset_required;
        }
        bool expected = true;
        return fake && fake->initialized.compare_exchange_strong(
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
    assert(::unlink(path) == 0);
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
    concurrent_submit_poll();
#if defined(RTFW_XDMA_LINUX_AVAILABLE)
    linux_adapter_smoke();
#endif
    return 0;
}

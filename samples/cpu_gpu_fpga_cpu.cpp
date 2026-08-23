#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <thread>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#include <rt/cuda_backend.hpp>
#include <rt/runtime.hpp>
#include <rt/xdma_backend.hpp>

namespace rtfw_sample_allocation {

std::atomic<bool> tracking{false};
std::atomic<std::size_t> count{0};

void record() noexcept {
    if (tracking.load(std::memory_order_relaxed)) {
        count.fetch_add(1, std::memory_order_relaxed);
    }
}

void* allocate(std::size_t bytes) {
    if (bytes == 0) {
        bytes = 1;
    }
    if (void* storage = std::malloc(bytes)) {
        return storage;
    }
    throw std::bad_alloc();
}

void* allocate_aligned(std::size_t bytes, std::size_t alignment) {
    alignment = std::max(alignment, alignof(std::max_align_t));
    if (bytes == 0) {
        bytes = alignment;
    }
    const auto remainder = bytes % alignment;
    if (remainder != 0) {
        if (bytes > std::numeric_limits<std::size_t>::max() -
                        (alignment - remainder)) {
            throw std::bad_alloc();
        }
        bytes += alignment - remainder;
    }
#if defined(_MSC_VER)
    if (void* storage = _aligned_malloc(bytes, alignment)) {
        return storage;
    }
    throw std::bad_alloc();
#else
    void* storage = nullptr;
    if (posix_memalign(&storage, alignment, bytes) == 0) {
        return storage;
    }
    throw std::bad_alloc();
#endif
}

void deallocate_aligned(void* storage) noexcept {
#if defined(_MSC_VER)
    _aligned_free(storage);
#else
    std::free(storage);
#endif
}

void begin() noexcept {
    count.store(0, std::memory_order_relaxed);
    tracking.store(true, std::memory_order_release);
}

std::size_t end() noexcept {
    tracking.store(false, std::memory_order_release);
    return count.load(std::memory_order_acquire);
}

} // namespace rtfw_sample_allocation

void* operator new(std::size_t bytes) {
    rtfw_sample_allocation::record();
    return rtfw_sample_allocation::allocate(bytes);
}

void* operator new[](std::size_t bytes) {
    rtfw_sample_allocation::record();
    return rtfw_sample_allocation::allocate(bytes);
}

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    rtfw_sample_allocation::record();
    return rtfw_sample_allocation::allocate_aligned(
        bytes, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    rtfw_sample_allocation::record();
    return rtfw_sample_allocation::allocate_aligned(
        bytes, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](bytes);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t bytes, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t bytes, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* storage) noexcept {
    std::free(storage);
}

void operator delete[](void* storage) noexcept {
    std::free(storage);
}

void operator delete(void* storage, std::size_t) noexcept {
    std::free(storage);
}

void operator delete[](void* storage, std::size_t) noexcept {
    std::free(storage);
}

void operator delete(void* storage, std::align_val_t) noexcept {
    rtfw_sample_allocation::deallocate_aligned(storage);
}

void operator delete[](void* storage, std::align_val_t) noexcept {
    rtfw_sample_allocation::deallocate_aligned(storage);
}

void operator delete(void* storage, std::size_t, std::align_val_t) noexcept {
    rtfw_sample_allocation::deallocate_aligned(storage);
}

void operator delete[](void* storage, std::size_t,
                       std::align_val_t) noexcept {
    rtfw_sample_allocation::deallocate_aligned(storage);
}

namespace rtfw_combined_sample {

inline constexpr std::size_t frame_count = 2;
inline constexpr std::size_t element_count = 8;
inline constexpr std::size_t payload_bytes =
    element_count * sizeof(std::uint32_t);
inline constexpr std::uint16_t graph_id = 7;
inline constexpr std::uint32_t trigger_offset = 4;
inline constexpr std::uint32_t event_index = 0;
inline constexpr std::uint64_t card_offset = 64;
inline constexpr std::uint64_t success_timeout_ns = 500'000'000;
inline constexpr std::uint64_t failure_timeout_ns = 100'000'000;
inline constexpr std::size_t trace_capacity = 96;

enum class FailureMode : std::uint8_t {
    none,
    cuda_graph,
    xdma_event_timeout,
    malformed_cuda_signal,
    malformed_cuda_timeout,
    bridge_recovery,
};

std::uint32_t read_u32(std::span<const std::byte> bytes,
                       std::size_t index) noexcept {
    const auto offset = index * sizeof(std::uint32_t);
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

void write_u32(std::span<std::byte> bytes, std::size_t index,
               std::uint32_t value) noexcept {
    const auto offset = index * sizeof(std::uint32_t);
    bytes[offset] = static_cast<std::byte>(value);
    bytes[offset + 1] = static_cast<std::byte>(value >> 8u);
    bytes[offset + 2] = static_cast<std::byte>(value >> 16u);
    bytes[offset + 3] = static_cast<std::byte>(value >> 24u);
}

std::uint32_t input_value(std::uint64_t frame,
                          std::size_t index) noexcept {
    return static_cast<std::uint32_t>(frame * 100u + index);
}

std::uint32_t cuda_value(std::uint32_t input) noexcept {
    return input * 2u + 3u;
}

std::uint32_t trigger_value(std::uint64_t frame) noexcept {
    return static_cast<std::uint32_t>(frame * 11u + 5u);
}

std::uint32_t output_value(std::uint64_t frame,
                           std::size_t index) noexcept {
    return cuda_value(input_value(frame, index)) + trigger_value(frame);
}

struct OperationTrace {
    std::array<char, trace_capacity> codes{};
    std::array<std::thread::id, trace_capacity> threads{};
    std::atomic<std::size_t> count{0};
    std::atomic<bool> overflow{false};

    void record(char code) noexcept {
        const auto index = count.fetch_add(1, std::memory_order_relaxed);
        if (index >= codes.size()) {
            overflow.store(true, std::memory_order_relaxed);
            return;
        }
        codes[index] = code;
        threads[index] = std::this_thread::get_id();
    }

    bool matches(std::span<const char> expected) const noexcept {
        if (overflow.load(std::memory_order_acquire) ||
            count.load(std::memory_order_acquire) != expected.size()) {
            return false;
        }
        return std::equal(expected.begin(), expected.end(), codes.begin());
    }
};

struct CudaDriver {
    static constexpr rt::CudaContext context = 0xc001u;
    static constexpr rt::CudaStream stream = 0x5101u;

    struct Event {
        std::atomic<bool> allocated{false};
        std::atomic<bool> ready{false};
    };

    struct Allocation {
        bool allocated = false;
        alignas(64) std::array<std::byte, payload_bytes> bytes{};
    };

    OperationTrace* operations = nullptr;
    rt::CudaGraphExec graph = 0;
    const void* expected_host = nullptr;
    std::atomic<bool> fail_graph{false};
    std::atomic<std::uint64_t> now{1};
    std::array<Event, 4> events{};
    std::array<Allocation, 2> allocations{};
    std::atomic<rt::CudaDeviceAddress> active_address{0};
    std::atomic<std::uint64_t> uploads{0};
    std::atomic<std::uint64_t> graph_launches{0};
    std::atomic<std::uint64_t> downloads{0};
    std::atomic<std::uint64_t> event_records{0};
    std::atomic<std::uint64_t> event_queries{0};
    std::atomic<std::uint64_t> host_registrations{0};
    std::atomic<std::uint64_t> host_unregistrations{0};
    std::atomic<std::uint64_t> frees{0};
    std::atomic<std::uint64_t> stream_synchronizations{0};
    std::array<std::thread::id, trace_capacity> query_threads{};

    explicit CudaDriver(OperationTrace& trace,
                        rt::CudaGraphExec graph_handle) noexcept
        : operations(&trace), graph(graph_handle) {}

    static CudaDriver* self(void* user_data) noexcept {
        return static_cast<CudaDriver*>(user_data);
    }

    static rt::CudaDriverResult push_context(
        void* user_data, rt::CudaContext requested) noexcept {
        return user_data && requested == context
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::context_lost;
    }

    static rt::CudaDriverResult pop_context(
        void* user_data, rt::CudaContext* output) noexcept {
        if (!user_data || !output) {
            return rt::CudaDriverResult::invalid_value;
        }
        *output = context;
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult event_create(
        void* user_data, rt::CudaEvent* output) noexcept {
        auto* driver = self(user_data);
        if (!driver || !output) {
            return rt::CudaDriverResult::invalid_value;
        }
        for (std::size_t index = 0; index < driver->events.size(); ++index) {
            bool expected = false;
            if (driver->events[index].allocated.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                driver->events[index].ready.store(false,
                                                  std::memory_order_relaxed);
                *output = static_cast<rt::CudaEvent>(index + 1);
                return rt::CudaDriverResult::success;
            }
        }
        return rt::CudaDriverResult::out_of_memory;
    }

    static Event* event_for(CudaDriver& driver,
                            rt::CudaEvent event) noexcept {
        if (event == 0 || event > driver.events.size()) {
            return nullptr;
        }
        auto& result = driver.events[static_cast<std::size_t>(event - 1)];
        return result.allocated.load(std::memory_order_acquire)
            ? &result
            : nullptr;
    }

    static rt::CudaDriverResult event_destroy(
        void* user_data, rt::CudaEvent event) noexcept {
        auto* driver = self(user_data);
        auto* state = driver ? event_for(*driver, event) : nullptr;
        if (!state) {
            return rt::CudaDriverResult::invalid_value;
        }
        state->ready.store(false, std::memory_order_relaxed);
        state->allocated.store(false, std::memory_order_release);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult event_record(
        void* user_data, rt::CudaEvent event, rt::CudaStream requested) noexcept {
        auto* driver = self(user_data);
        auto* state = driver ? event_for(*driver, event) : nullptr;
        if (!state || requested != stream) {
            return rt::CudaDriverResult::invalid_value;
        }
        driver->operations->record('E');
        driver->event_records.fetch_add(1, std::memory_order_relaxed);
        state->ready.store(true, std::memory_order_release);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult event_query(
        void* user_data, rt::CudaEvent event) noexcept {
        auto* driver = self(user_data);
        auto* state = driver ? event_for(*driver, event) : nullptr;
        if (!state) {
            return rt::CudaDriverResult::invalid_value;
        }
        const auto index = driver->event_queries.fetch_add(
            1, std::memory_order_relaxed);
        if (index < driver->query_threads.size()) {
            driver->query_threads[static_cast<std::size_t>(index)] =
                std::this_thread::get_id();
        }
        return state->ready.load(std::memory_order_acquire)
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::not_ready;
    }

    static rt::CudaDriverResult event_synchronize(
        void* user_data, rt::CudaEvent event) noexcept {
        auto* driver = self(user_data);
        auto* state = driver ? event_for(*driver, event) : nullptr;
        if (!state) {
            return rt::CudaDriverResult::invalid_value;
        }
        state->ready.store(true, std::memory_order_release);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult stream_synchronize(
        void* user_data, rt::CudaStream requested) noexcept {
        auto* driver = self(user_data);
        if (!driver || requested != stream) {
            return rt::CudaDriverResult::invalid_value;
        }
        for (auto& event : driver->events) {
            if (event.allocated.load(std::memory_order_acquire)) {
                event.ready.store(true, std::memory_order_release);
            }
        }
        driver->stream_synchronizations.fetch_add(1,
                                                  std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult mem_alloc(
        void* user_data, std::uint64_t bytes,
        rt::CudaDeviceAddress* output) noexcept {
        auto* driver = self(user_data);
        if (!driver || !output || bytes != payload_bytes) {
            return rt::CudaDriverResult::invalid_value;
        }
        for (auto& allocation : driver->allocations) {
            if (!allocation.allocated) {
                allocation.allocated = true;
                *output = static_cast<rt::CudaDeviceAddress>(
                    reinterpret_cast<std::uintptr_t>(allocation.bytes.data()));
                return rt::CudaDriverResult::success;
            }
        }
        return rt::CudaDriverResult::out_of_memory;
    }

    static rt::CudaDriverResult mem_free(
        void* user_data, rt::CudaDeviceAddress address) noexcept {
        auto* driver = self(user_data);
        if (!driver || address == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        for (auto& allocation : driver->allocations) {
            if (allocation.allocated &&
                reinterpret_cast<std::uintptr_t>(allocation.bytes.data()) ==
                    static_cast<std::uintptr_t>(address)) {
                allocation.allocated = false;
                driver->frees.fetch_add(1, std::memory_order_relaxed);
                return rt::CudaDriverResult::success;
            }
        }
        return rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult host_register(
        void* user_data, void* address, std::uint64_t bytes) noexcept {
        auto* driver = self(user_data);
        if (!driver || address != driver->expected_host ||
            bytes != payload_bytes) {
            return rt::CudaDriverResult::invalid_value;
        }
        driver->host_registrations.fetch_add(1, std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult host_unregister(
        void* user_data, void* address) noexcept {
        auto* driver = self(user_data);
        if (!driver || address != driver->expected_host) {
            return rt::CudaDriverResult::invalid_value;
        }
        driver->host_unregistrations.fetch_add(1,
                                               std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult copy_host_to_device(
        void* user_data, rt::CudaDeviceAddress destination,
        const void* source, std::uint64_t bytes,
        rt::CudaStream requested) noexcept {
        auto* driver = self(user_data);
        if (!driver || destination == 0 || source != driver->expected_host ||
            bytes != payload_bytes || requested != stream) {
            return rt::CudaDriverResult::invalid_value;
        }
        driver->operations->record('H');
        driver->uploads.fetch_add(1, std::memory_order_relaxed);
        driver->active_address.store(destination, std::memory_order_release);
        std::memcpy(reinterpret_cast<void*>(
                        static_cast<std::uintptr_t>(destination)),
                    source, static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult copy_device_to_host(
        void* user_data, void* destination, rt::CudaDeviceAddress source,
        std::uint64_t bytes, rt::CudaStream requested) noexcept {
        auto* driver = self(user_data);
        if (!driver || destination != driver->expected_host || source == 0 ||
            bytes != payload_bytes || requested != stream) {
            return rt::CudaDriverResult::invalid_value;
        }
        driver->operations->record('D');
        driver->downloads.fetch_add(1, std::memory_order_relaxed);
        std::memcpy(destination,
                    reinterpret_cast<const void*>(
                        static_cast<std::uintptr_t>(source)),
                    static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult copy_device_to_device(
        void*, rt::CudaDeviceAddress destination, rt::CudaDeviceAddress source,
        std::uint64_t bytes, rt::CudaStream) noexcept {
        if (destination == 0 || source == 0 || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        std::memmove(reinterpret_cast<void*>(
                         static_cast<std::uintptr_t>(destination)),
                     reinterpret_cast<const void*>(
                         static_cast<std::uintptr_t>(source)),
                     static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult memset_device(
        void*, rt::CudaDeviceAddress destination, std::uint8_t value,
        std::uint64_t bytes, rt::CudaStream) noexcept {
        if (destination == 0 || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        std::memset(reinterpret_cast<void*>(
                        static_cast<std::uintptr_t>(destination)),
                    value, static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult launch_kernel(
        void* user_data, rt::CudaFunction, std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
        std::uint32_t, rt::CudaStream, void* const*) noexcept {
        return user_data ? rt::CudaDriverResult::success
                         : rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult graph_launch(
        void* user_data, rt::CudaGraphExec requested,
        rt::CudaStream requested_stream) noexcept {
        auto* driver = self(user_data);
        if (!driver || requested != driver->graph ||
            requested_stream != stream) {
            return rt::CudaDriverResult::invalid_value;
        }
        driver->operations->record('G');
        driver->graph_launches.fetch_add(1, std::memory_order_relaxed);
        if (driver->fail_graph.exchange(false, std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::launch_failure;
        }
        const auto address = driver->active_address.load(
            std::memory_order_acquire);
        if (address == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        auto bytes = std::span<std::byte>(
            reinterpret_cast<std::byte*>(
                static_cast<std::uintptr_t>(address)),
            payload_bytes);
        for (std::size_t index = 0; index < element_count; ++index) {
            write_u32(bytes, index, cuda_value(read_u32(bytes, index)));
        }
        return rt::CudaDriverResult::success;
    }

    static std::uint64_t monotonic(void* user_data) noexcept {
        auto* driver = self(user_data);
        return driver ? driver->now.fetch_add(1, std::memory_order_relaxed) : 0;
    }

    rt::CudaDriverApi api() noexcept {
        rt::CudaDriverApi result;
        result.struct_size = rt::cuda_driver_api_v2_struct_size;
        result.api_version = rt::cuda_driver_api_version_2;
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
        result.memcpy_host_to_device_async = &copy_host_to_device;
        result.memcpy_device_to_host_async = &copy_device_to_host;
        result.memcpy_device_to_device_async = &copy_device_to_device;
        result.memset_d8_async = &memset_device;
        result.launch_kernel = &launch_kernel;
        result.monotonic_time_ns = &monotonic;
        result.graph_launch = &graph_launch;
        return result;
    }
};

struct XdmaDriver {
    OperationTrace* operations = nullptr;
    void* expected_host = nullptr;
    std::array<std::byte, 256> card{};
    std::array<std::uint32_t, 4> control{};
    std::atomic<bool> initialized{false};
    std::atomic<bool> block_event{false};
    std::atomic<bool> event_entered{false};
    std::atomic<bool> event_exited{false};
    std::atomic<bool> fail_shutdown_once{false};
    std::atomic<std::uint64_t> now{1};
    std::atomic<std::uint64_t> h2c_calls{0};
    std::atomic<std::uint64_t> c2h_calls{0};
    std::atomic<std::uint64_t> control_writes{0};
    std::atomic<std::uint64_t> event_waits{0};
    std::atomic<std::uint64_t> stop_requests{0};
    std::atomic<std::uint64_t> shutdown_calls{0};

    explicit XdmaDriver(OperationTrace& trace) noexcept : operations(&trace) {}

    static XdmaDriver* self(void* user_data) noexcept {
        return static_cast<XdmaDriver*>(user_data);
    }

    static rt::XdmaDriverResult initialize(void* user_data) noexcept {
        auto* driver = self(user_data);
        bool expected = false;
        return driver && driver->initialized.compare_exchange_strong(
                             expected, true, std::memory_order_acq_rel,
                             std::memory_order_relaxed)
            ? rt::XdmaDriverResult::success
            : rt::XdmaDriverResult::invalid_value;
    }

    static rt::XdmaTransferResult transfer(
        void* user_data, rt::XdmaDirection direction, std::uint32_t channel,
        std::uint64_t offset, void* host, std::uint64_t bytes) noexcept {
        auto* driver = self(user_data);
        rt::XdmaTransferResult result;
        if (!driver || host != driver->expected_host || channel != 0 ||
            offset != card_offset || bytes != payload_bytes ||
            card_offset + payload_bytes > driver->card.size()) {
            result.result = rt::XdmaDriverResult::invalid_value;
            return result;
        }
        auto* device = driver->card.data() + card_offset;
        if (direction == rt::XdmaDirection::host_to_card) {
            driver->operations->record('h');
            driver->h2c_calls.fetch_add(1, std::memory_order_relaxed);
            std::memcpy(device, host, payload_bytes);
        } else {
            driver->operations->record('c');
            driver->c2h_calls.fetch_add(1, std::memory_order_relaxed);
            std::memcpy(host, device, payload_bytes);
        }
        driver->now.fetch_add(1, std::memory_order_relaxed);
        result.result = rt::XdmaDriverResult::success;
        result.bytes_transferred = bytes;
        return result;
    }

    static rt::XdmaDriverResult reset(void* user_data) noexcept {
        auto* driver = self(user_data);
        return driver && driver->initialized.load(std::memory_order_acquire)
            ? rt::XdmaDriverResult::success
            : rt::XdmaDriverResult::invalid_value;
    }

    static rt::XdmaDriverResult shutdown(void* user_data) noexcept {
        auto* driver = self(user_data);
        if (!driver) {
            return rt::XdmaDriverResult::invalid_value;
        }
        driver->shutdown_calls.fetch_add(1, std::memory_order_relaxed);
        if (driver->fail_shutdown_once.exchange(false,
                                                std::memory_order_acq_rel)) {
            return rt::XdmaDriverResult::reset_required;
        }
        bool expected = true;
        return driver->initialized.compare_exchange_strong(
                   expected, false, std::memory_order_acq_rel,
                   std::memory_order_relaxed)
            ? rt::XdmaDriverResult::success
            : rt::XdmaDriverResult::invalid_value;
    }

    static std::uint64_t monotonic(void* user_data) noexcept {
        auto* driver = self(user_data);
        return driver ? driver->now.fetch_add(1, std::memory_order_relaxed) : 0;
    }

    static rt::XdmaControlReadResult control_read(
        void* user_data, std::uint32_t offset) noexcept {
        auto* driver = self(user_data);
        rt::XdmaControlReadResult result;
        if (!driver || (offset & 3u) != 0 || offset / 4u >=
                                                    driver->control.size()) {
            result.result = rt::XdmaDriverResult::invalid_value;
            return result;
        }
        result.result = rt::XdmaDriverResult::success;
        result.value = driver->control[offset / 4u];
        return result;
    }

    static rt::XdmaDriverResult control_write(
        void* user_data, std::uint32_t offset, std::uint32_t value) noexcept {
        auto* driver = self(user_data);
        if (!driver || offset != trigger_offset ||
            offset / 4u >= driver->control.size()) {
            return rt::XdmaDriverResult::invalid_value;
        }
        driver->operations->record('w');
        driver->control_writes.fetch_add(1, std::memory_order_relaxed);
        driver->control[offset / 4u] = value;
        return rt::XdmaDriverResult::success;
    }

    static rt::XdmaUserEventResult wait_event(
        void* user_data, std::uint32_t requested_event,
        std::uint64_t timeout_ns) noexcept {
        auto* driver = self(user_data);
        rt::XdmaUserEventResult result;
        if (!driver || requested_event != event_index || timeout_ns == 0) {
            result.result = rt::XdmaDriverResult::invalid_value;
            return result;
        }
        driver->operations->record('e');
        driver->event_waits.fetch_add(1, std::memory_order_relaxed);
        driver->event_entered.store(true, std::memory_order_release);
        while (driver->block_event.load(std::memory_order_acquire)) {
            driver->block_event.wait(true, std::memory_order_relaxed);
        }
        driver->event_exited.store(true, std::memory_order_release);
        if (driver->stop_requests.load(std::memory_order_acquire) != 0) {
            result.result = rt::XdmaDriverResult::timeout;
            return result;
        }
        auto bytes = std::span<std::byte>(driver->card.data() + card_offset,
                                          payload_bytes);
        const auto trigger = driver->control[trigger_offset / 4u];
        for (std::size_t index = 0; index < element_count; ++index) {
            write_u32(bytes, index, read_u32(bytes, index) + trigger);
        }
        result.result = rt::XdmaDriverResult::success;
        result.value = 0xe000'0000u | trigger;
        return result;
    }

    static rt::XdmaDriverResult request_stop(void* user_data) noexcept {
        auto* driver = self(user_data);
        if (!driver) {
            return rt::XdmaDriverResult::invalid_value;
        }
        driver->stop_requests.fetch_add(1, std::memory_order_relaxed);
        driver->block_event.store(false, std::memory_order_release);
        driver->block_event.notify_all();
        return rt::XdmaDriverResult::success;
    }

    rt::XdmaDriverApi api() noexcept {
        rt::XdmaDriverApi result;
        result.struct_size = sizeof(result);
        result.api_version = rt::xdma_driver_api_version_2;
        result.user_data = this;
        result.initialize = &initialize;
        result.transfer = &transfer;
        result.reset = &reset;
        result.shutdown = &shutdown;
        result.monotonic_time_ns = &monotonic;
        result.control_read32 = &control_read;
        result.control_write32 = &control_write;
        result.wait_user_event = &wait_event;
        result.request_stop = &request_stop;
        return result;
    }
};

rt::CudaBackendConfig cuda_config(
    std::span<const rt::CudaStream> streams) noexcept {
    rt::CudaBackendConfig result;
    result.queue_capacity = 4;
    result.buffer_capacity = 2;
    result.kernel_capacity = 1;
    result.context = CudaDriver::context;
    result.streams = streams;
    result.allocate_device_mirrors = true;
    result.register_host_memory = true;
    return result;
}

rt::XdmaBackendConfig xdma_config() noexcept {
    rt::XdmaBackendConfig result;
    result.queue_capacity = 4;
    result.buffer_capacity = 2;
    result.worker_count = 2;
    result.h2c_channel_count = 1;
    result.c2h_channel_count = 1;
    result.max_transfer_bytes = payload_bytes;
    result.max_buffer_bytes = payload_bytes;
    result.transfer_alignment = alignof(std::uint32_t);
    result.control_aperture_bytes = 16;
    result.user_event_count = 1;
    return result;
}

rt::RuntimeConfig runtime_config() noexcept {
    rt::RuntimeConfig result;
    result.callback_capacity = 8;
    result.scratch_bytes = 4096;
    result.trace_capacity = 256;
    result.worker_count = 2;
    result.executor_queue_capacity = 16;
    result.task_scratch_bytes = 256;
    result.task_scratch_slots = 16;
    result.device_backend_capacity = 2;
    result.device_buffer_capacity = 2;
    result.device_outstanding_capacity = 4;
    result.device_completion_batch = 4;
    return result;
}

rt::HalV2BufferReference buffer_reference(
    rt::DeviceBufferHandle buffer, std::uint32_t access,
    std::uint64_t bytes = payload_bytes,
    std::uint64_t offset = 0) noexcept {
    rt::HalV2BufferReference result;
    result.buffer_token = buffer.value;
    result.access = access;
    result.offset = offset;
    result.bytes = bytes;
    return result;
}

void set_xdma_transfer(rt::DeviceCommand& command,
                       rt::XdmaDirection direction,
                       rt::DeviceBufferHandle buffer) noexcept {
    command = {};
    command.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    command.opcode = direction == rt::XdmaDirection::host_to_card
        ? rt::xdma_device_opcode_host_to_card
        : rt::xdma_device_opcode_card_to_host;
    rt::XdmaTransfer transfer;
    transfer.device_offset = card_offset;
    transfer.channel = 0;
    command.payload_size = sizeof(transfer);
    std::memcpy(command.payload.data(), &transfer, sizeof(transfer));
    command.buffer_count = 1;
    command.buffers[0] = buffer_reference(
        buffer,
        direction == rt::XdmaDirection::host_to_card
            ? RTFW_DEVICE_ACCESS_READ
            : RTFW_DEVICE_ACCESS_WRITE);
}

struct TimelineObservation {
    std::uint64_t cuda_accepted = 0;
    std::uint64_t cuda_completed = 0;
    std::uint64_t xdma_accepted = 0;
    std::uint64_t xdma_completed = 0;
};

struct Scenario {
    FailureMode failure = FailureMode::none;
    std::thread::id control_thread = std::this_thread::get_id();
    OperationTrace operations{};
    alignas(64) std::array<std::byte, payload_bytes> cuda_stage{};
    alignas(64) std::array<std::byte, payload_bytes> xdma_stage{};
    std::array<rt::CudaGraphBufferBinding, 1> graph_bindings{
        rt::CudaGraphBufferBinding{"combined.cuda.staging",
                                   RTFW_DEVICE_ACCESS_READ_WRITE}};
    std::array<rt::CudaStream, 1> streams{CudaDriver::stream};
    CudaDriver cuda_driver;
    rt::CudaDeviceBackend cuda_backend;
    XdmaDriver xdma_driver;
    rt::XdmaDeviceBackend xdma_backend;
    rt::Runtime runtime;

    rt::DeviceBackendHandle cuda_backend_handle{};
    rt::DeviceBackendHandle xdma_backend_handle{};
    rt::DeviceMemoryDomainHandle cuda_domain{};
    rt::DeviceMemoryDomainHandle xdma_domain{};
    rt::DeviceBufferHandle cuda_buffer{};
    rt::DeviceBufferHandle xdma_buffer{};
    rt::DeviceTimelineHandle cuda_timeline{};
    rt::DeviceTimelineHandle xdma_timeline{};
    rt::PhaseHandle prepare_phase{};
    rt::PhaseHandle cuda_phase{};
    rt::PhaseHandle bridge_phase{};
    rt::PhaseHandle xdma_phase{};
    rt::PhaseHandle validate_phase{};
    rt::DeviceCommandBatch cuda_declaration{};
    rt::DeviceCommandBatch xdma_declaration{};
    std::array<TimelineObservation, frame_count> timeline_observations{};
    std::array<std::byte, payload_bytes> expected_final{};
    std::atomic<std::uint64_t> prepare_calls{0};
    std::atomic<std::uint64_t> cuda_provider_calls{0};
    std::atomic<std::uint64_t> bridge_calls{0};
    std::atomic<std::uint64_t> xdma_provider_calls{0};
    std::atomic<std::uint64_t> validate_calls{0};
    std::atomic<bool> bridge_exact{true};
    std::size_t measured_allocations = 0;
    std::size_t completed_frames = 0;
    std::size_t executor_instances = 0;
    std::size_t submission_instances = 0;
    std::size_t service_instances = 0;
    rt::MemoryPlan memory_plan{};
    bool configuration_contract_valid = false;

    explicit Scenario(FailureMode requested = FailureMode::none,
                      std::uint64_t instance = 1)
        : failure(requested),
          cuda_driver(operations,
                      static_cast<rt::CudaGraphExec>(0x9000u + instance)),
          cuda_backend(cuda_driver.api(), cuda_config(streams)),
          xdma_driver(operations),
          xdma_backend(xdma_driver.api(), xdma_config()) {
        cuda_driver.expected_host = cuda_stage.data();
        xdma_driver.expected_host = xdma_stage.data();
        cuda_driver.fail_graph.store(failure == FailureMode::cuda_graph,
                                     std::memory_order_relaxed);
        xdma_driver.block_event.store(
            failure == FailureMode::xdma_event_timeout,
            std::memory_order_relaxed);
    }

    Scenario(const Scenario&) = delete;
    Scenario& operator=(const Scenario&) = delete;

    static rt::CallbackResult prepare(
        void* user_data, const rt::CallbackContext& context) noexcept {
        auto& self = *static_cast<Scenario*>(user_data);
        self.operations.record('P');
        self.prepare_calls.fetch_add(1, std::memory_order_relaxed);
        std::fill(self.xdma_stage.begin(), self.xdma_stage.end(), std::byte{});
        for (std::size_t index = 0; index < element_count; ++index) {
            write_u32(self.cuda_stage, index,
                      input_value(context.frame.frame_index, index));
            write_u32(self.expected_final, index,
                      output_value(context.frame.frame_index, index));
        }
        return rt::CallbackResult::ok;
    }

    static rt::CallbackResult provide_cuda(
        void* user_data, const rt::DeviceCallbackContext& context,
        rt::DeviceCommandBatch& batch) noexcept {
        auto& self = *static_cast<Scenario*>(user_data);
        self.operations.record('U');
        self.cuda_provider_calls.fetch_add(1, std::memory_order_relaxed);
        batch = self.cuda_declaration;
        batch.timeout_ns = self.failure == FailureMode::malformed_cuda_timeout
            ? 0
            : success_timeout_ns;
        batch.signals[0].value = context.frame.frame_index;
        if (self.failure == FailureMode::malformed_cuda_signal) {
            batch.signals[0].timeline_handle = self.xdma_timeline.value;
        }
        return rt::CallbackResult::ok;
    }

    static rt::CallbackResult bridge(
        void* user_data, const rt::CallbackContext& context) noexcept {
        auto& self = *static_cast<Scenario*>(user_data);
        self.operations.record('B');
        self.bridge_calls.fetch_add(1, std::memory_order_relaxed);
        const auto xdma_calls_before =
            self.xdma_driver.h2c_calls.load(std::memory_order_acquire) +
            self.xdma_driver.c2h_calls.load(std::memory_order_acquire) +
            self.xdma_driver.control_writes.load(std::memory_order_acquire) +
            self.xdma_driver.event_waits.load(std::memory_order_acquire);
        const auto cuda_calls_before =
            self.cuda_driver.uploads.load(std::memory_order_acquire) +
            self.cuda_driver.graph_launches.load(std::memory_order_acquire) +
            self.cuda_driver.downloads.load(std::memory_order_acquire) +
            self.cuda_driver.event_records.load(std::memory_order_acquire);
        for (std::size_t index = 0; index < element_count; ++index) {
            if (read_u32(self.cuda_stage, index) !=
                cuda_value(input_value(context.frame.frame_index, index))) {
                self.bridge_exact.store(false, std::memory_order_release);
                return rt::CallbackResult::error;
            }
        }
        if (self.failure == FailureMode::bridge_recovery) {
            return rt::CallbackResult::error;
        }
        std::memcpy(self.xdma_stage.data(), self.cuda_stage.data(),
                    self.cuda_stage.size());
        const auto xdma_calls_after =
            self.xdma_driver.h2c_calls.load(std::memory_order_acquire) +
            self.xdma_driver.c2h_calls.load(std::memory_order_acquire) +
            self.xdma_driver.control_writes.load(std::memory_order_acquire) +
            self.xdma_driver.event_waits.load(std::memory_order_acquire);
        const auto cuda_calls_after =
            self.cuda_driver.uploads.load(std::memory_order_acquire) +
            self.cuda_driver.graph_launches.load(std::memory_order_acquire) +
            self.cuda_driver.downloads.load(std::memory_order_acquire) +
            self.cuda_driver.event_records.load(std::memory_order_acquire);
        if (xdma_calls_before != xdma_calls_after ||
            cuda_calls_before != cuda_calls_after) {
            self.bridge_exact.store(false, std::memory_order_release);
            return rt::CallbackResult::error;
        }
        return rt::CallbackResult::ok;
    }

    static rt::CallbackResult provide_xdma(
        void* user_data, const rt::DeviceCallbackContext& context,
        rt::DeviceCommandBatch& batch) noexcept {
        auto& self = *static_cast<Scenario*>(user_data);
        self.operations.record('X');
        self.xdma_provider_calls.fetch_add(1, std::memory_order_relaxed);
        batch = self.xdma_declaration;
        batch.timeout_ns = self.failure == FailureMode::xdma_event_timeout
            ? failure_timeout_ns
            : success_timeout_ns;
        batch.signals[0].value = context.frame.frame_index;
        if (!rt::set_xdma_control_write(
                batch.commands[1], trigger_offset,
                trigger_value(context.frame.frame_index))) {
            return rt::CallbackResult::error;
        }
        return rt::CallbackResult::ok;
    }

    static rt::CallbackResult validate(
        void* user_data, const rt::CallbackContext&) noexcept {
        auto& self = *static_cast<Scenario*>(user_data);
        self.operations.record('V');
        self.validate_calls.fetch_add(1, std::memory_order_relaxed);
        return std::equal(self.xdma_stage.begin(), self.xdma_stage.end(),
                          self.expected_final.begin())
            ? rt::CallbackResult::ok
            : rt::CallbackResult::error;
    }

    rt::Status configure() noexcept {
        if (cuda_backend.register_graph(graph_id, cuda_driver.graph,
                                        graph_bindings) !=
            RTFW_DEVICE_STATUS_OK) {
            return rt::Status::device_error;
        }
        auto status = runtime.configure(runtime_config());
        if (status != rt::Status::ok) {
            return status;
        }
        const auto cuda_registration =
            cuda_backend.hal_v2_registration("combined.cuda");
        const auto xdma_registration =
            xdma_backend.hal_v2_registration("combined.xdma");
        status = runtime.register_device_backend(cuda_registration,
                                                 cuda_backend_handle);
        if (status != rt::Status::ok) {
            return status;
        }
        status = runtime.register_device_backend(xdma_registration,
                                                 xdma_backend_handle);
        if (status != rt::Status::ok) {
            return status;
        }

        rt::HalV2MemoryDomain cuda_descriptor;
        rt::HalV2MemoryDomain xdma_descriptor;
        if (!runtime.device_memory_domain_at(cuda_backend_handle, 1,
                                             cuda_domain, cuda_descriptor) ||
            !runtime.device_memory_domain_at(xdma_backend_handle, 0,
                                             xdma_domain, xdma_descriptor)) {
            return rt::Status::device_error;
        }
        const auto cuda_start = reinterpret_cast<std::uintptr_t>(
            cuda_stage.data());
        const auto xdma_start = reinterpret_cast<std::uintptr_t>(
            xdma_stage.data());
        const bool disjoint =
            cuda_start + cuda_stage.size() <= xdma_start ||
            xdma_start + xdma_stage.size() <= cuda_start;
        configuration_contract_valid =
            disjoint && cuda_descriptor.coherency == static_cast<std::uint32_t>(
                rt::HalV2MemoryCoherency::staged_copy) &&
            cuda_descriptor.required_synchronization ==
                (rt::hal_v2_memory_sync_copy_to_device |
                 rt::hal_v2_memory_sync_copy_from_device) &&
            xdma_descriptor.coherency == static_cast<std::uint32_t>(
                rt::HalV2MemoryCoherency::host_coherent) &&
            xdma_descriptor.required_synchronization ==
                rt::hal_v2_memory_sync_none;
        if (!configuration_contract_valid) {
            return rt::Status::device_error;
        }

        const auto access = RTFW_DEVICE_BUFFER_HOST_READ |
                            RTFW_DEVICE_BUFFER_HOST_WRITE |
                            RTFW_DEVICE_BUFFER_DEVICE_READ |
                            RTFW_DEVICE_BUFFER_DEVICE_WRITE;
        status = runtime.register_device_buffer(
            {"combined.cuda.staging", cuda_backend_handle, cuda_domain,
             cuda_stage, {}, cuda_stage.size(),
             rt::HalV2MemoryOwnership::borrowed_host, access,
             rt::HalV2MemoryCoherency::staged_copy,
             rt::hal_v2_memory_sync_copy_to_device |
                 rt::hal_v2_memory_sync_copy_from_device},
            cuda_buffer);
        if (status != rt::Status::ok) {
            return status;
        }
        status = runtime.register_device_buffer(
            {"combined.xdma.staging", xdma_backend_handle, xdma_domain,
             xdma_stage, {}, xdma_stage.size(),
             rt::HalV2MemoryOwnership::borrowed_host, access,
             rt::HalV2MemoryCoherency::host_coherent,
             rt::hal_v2_memory_sync_none},
            xdma_buffer);
        if (status != rt::Status::ok) {
            return status;
        }
        status = runtime.register_device_timeline(
            {"combined.cuda.timeline", cuda_backend_handle, 0},
            cuda_timeline);
        if (status != rt::Status::ok) {
            return status;
        }
        status = runtime.register_device_timeline(
            {"combined.xdma.timeline", xdma_backend_handle, 0},
            xdma_timeline);
        if (status != rt::Status::ok) {
            return status;
        }

        cuda_declaration.command_count = 3;
        cuda_declaration.signal_count = 1;
        auto cuda_reference = buffer_reference(
            cuda_buffer, RTFW_DEVICE_ACCESS_READ_WRITE);
        auto& upload = cuda_declaration.commands[0];
        upload.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::copy);
        upload.operation = static_cast<std::uint32_t>(
            rt::HalV2MemoryOperation::copy_to_device);
        upload.source = cuda_reference;
        upload.source.access = RTFW_DEVICE_ACCESS_READ;
        upload.destination = cuda_reference;
        upload.destination.access = RTFW_DEVICE_ACCESS_WRITE;
        auto& graph = cuda_declaration.commands[1];
        graph.kind = static_cast<std::uint32_t>(
            rt::HalV2CommandKind::dispatch);
        graph.opcode = rt::cuda_device_opcode_graph(graph_id);
        graph.buffer_count = 1;
        graph.buffers[0] = cuda_reference;
        auto& download = cuda_declaration.commands[2];
        download = upload;
        download.operation = static_cast<std::uint32_t>(
            rt::HalV2MemoryOperation::copy_from_device);
        cuda_declaration.signals[0].timeline_handle = cuda_timeline.value;

        xdma_declaration.command_count = 4;
        xdma_declaration.signal_count = 1;
        set_xdma_transfer(xdma_declaration.commands[0],
                          rt::XdmaDirection::host_to_card, xdma_buffer);
        if (!rt::set_xdma_control_write(xdma_declaration.commands[1],
                                        trigger_offset, 0)) {
            return rt::Status::internal_error;
        }
        auto event_reference = buffer_reference(
            xdma_buffer, RTFW_DEVICE_ACCESS_WRITE, sizeof(std::uint32_t));
        if (!rt::set_xdma_user_event_wait(xdma_declaration.commands[2],
                                          event_index, event_reference)) {
            return rt::Status::internal_error;
        }
        set_xdma_transfer(xdma_declaration.commands[3],
                          rt::XdmaDirection::card_to_host, xdma_buffer);
        xdma_declaration.signals[0].timeline_handle = xdma_timeline.value;

        status = runtime.register_callback(
            {"combined.cpu.prepare", &prepare, this}, prepare_phase);
        if (status != rt::Status::ok) {
            return status;
        }
        status = runtime.register_device_batch_phase(
            {"combined.cuda.batch", cuda_backend_handle, &provide_cuda, this,
             cuda_declaration},
            cuda_phase);
        if (status != rt::Status::ok) {
            return status;
        }
        status = runtime.register_callback(
            {"combined.cpu.bridge", &bridge, this}, bridge_phase);
        if (status != rt::Status::ok) {
            return status;
        }
        status = runtime.register_device_batch_phase(
            {"combined.xdma.batch", xdma_backend_handle, &provide_xdma, this,
             xdma_declaration},
            xdma_phase);
        if (status != rt::Status::ok) {
            return status;
        }
        status = runtime.register_callback(
            {"combined.cpu.validate", &validate, this}, validate_phase);
        if (status != rt::Status::ok) {
            return status;
        }
        for (const auto dependency :
             std::array<std::array<rt::PhaseHandle, 2>, 4>{
                 std::array{prepare_phase, cuda_phase},
                 std::array{cuda_phase, bridge_phase},
                 std::array{bridge_phase, xdma_phase},
                 std::array{xdma_phase, validate_phase}}) {
            status = runtime.add_dependency(dependency[0], dependency[1]);
            if (status != rt::Status::ok) {
                return status;
            }
        }
        return rt::Status::ok;
    }

    rt::Status finalize() noexcept {
        const auto status = runtime.finalize();
        if (status != rt::Status::ok) {
            return status;
        }
        if (!runtime.memory_plan(memory_plan)) {
            return rt::Status::internal_error;
        }
        rt::CpuMemoryPolicyReport report;
        if (!runtime.cpu_memory_policy_report(report)) {
            return rt::Status::internal_error;
        }
        for (std::size_t index = 0; index < report.thread_count; ++index) {
            const auto& row = report.threads[index];
            if (row.role == rt::thread_role_executor_worker) {
                executor_instances = row.logical_instance_count;
            } else if (row.role == rt::thread_role_device_submission) {
                submission_instances = row.logical_instance_count;
            } else if (row.role == rt::thread_role_device_service) {
                service_instances = row.logical_instance_count;
            }
        }
        return rt::Status::ok;
    }

    rt::Status start() noexcept {
        return runtime.start();
    }

    bool capture_timelines(std::size_t index) noexcept {
        if (index >= timeline_observations.size()) {
            return false;
        }
        rt::DeviceTimelineInfo cuda;
        rt::DeviceTimelineInfo xdma;
        if (!runtime.device_timeline_at(cuda_backend_handle, 0, cuda) ||
            !runtime.device_timeline_at(xdma_backend_handle, 0, xdma) ||
            cuda.backend != cuda_backend_handle ||
            xdma.backend != xdma_backend_handle ||
            cuda.timeline == xdma.timeline || cuda_timeline == xdma_timeline) {
            return false;
        }
        timeline_observations[index] = {
            cuda.last_accepted_value, cuda.completed_value,
            xdma.last_accepted_value, xdma.completed_value};
        return true;
    }

    rt::Status run_from(std::uint64_t first_frame,
                        std::size_t frames) noexcept {
        completed_frames = 0;
        rtfw_sample_allocation::begin();
        rt::Status status = rt::Status::ok;
        for (std::size_t offset = 0; offset < frames; ++offset) {
            const auto frame = first_frame +
                static_cast<std::uint64_t>(offset);
            status = runtime.step(
                {frame, std::chrono::milliseconds(1), std::nullopt});
            const auto observation = static_cast<std::size_t>(
                std::min<std::uint64_t>(frame - 1, frame_count - 1));
            if (!capture_timelines(observation)) {
                status = rt::Status::internal_error;
            }
            if (status != rt::Status::ok) {
                break;
            }
            ++completed_frames;
        }
        measured_allocations = rtfw_sample_allocation::end();
        return status;
    }

    rt::Status run(std::size_t frames) noexcept {
        return run_from(1, frames);
    }

    rt::Status stop() noexcept {
        return runtime.stop();
    }

    bool trace_causality(std::size_t expected_batches) noexcept {
        std::array<rt::RuntimeTraceEvent, 256> events{};
        rt::RuntimeTraceCursor cursor;
        rt::RuntimeTraceReadResult result;
        if (runtime.read_trace(cursor, events, result) != rt::Status::ok ||
            result.lost_events != 0) {
            return false;
        }
        std::array<std::uint64_t, 16> submitted{};
        std::array<std::uint64_t, 16> completed{};
        std::size_t submitted_count = 0;
        std::size_t completed_count = 0;
        for (std::size_t index = 0; index < result.events_read; ++index) {
            const auto id = events[index].value;
            if (id == 0 || id >= submitted.size()) {
                continue;
            }
            if (events[index].type ==
                rt::RuntimeTraceEventType::device_submitted) {
                submitted[static_cast<std::size_t>(id)] =
                    events[index].sequence;
                ++submitted_count;
                if (events[index].producer !=
                        rt::RuntimeTraceProducer::worker ||
                    events[index].worker_index ==
                        std::numeric_limits<std::uint32_t>::max()) {
                    return false;
                }
            } else if (events[index].type ==
                       rt::RuntimeTraceEventType::device_completed) {
                completed[static_cast<std::size_t>(id)] =
                    events[index].sequence;
                ++completed_count;
                if (events[index].producer !=
                    rt::RuntimeTraceProducer::device_service) {
                    return false;
                }
            }
        }
        if (submitted_count != expected_batches ||
            completed_count != expected_batches) {
            return false;
        }
        for (std::size_t id = 1; id < submitted.size(); ++id) {
            if ((submitted[id] == 0) != (completed[id] == 0) ||
                (submitted[id] != 0 && submitted[id] >= completed[id])) {
                return false;
            }
        }
        return true;
    }

    bool threads_are_bounded_and_separated() const noexcept {
        const auto operation_count = operations.count.load(
            std::memory_order_acquire);
        if (operation_count > operations.codes.size() ||
            cuda_driver.event_queries.load(std::memory_order_acquire) == 0) {
            return false;
        }
        std::array<std::thread::id, 2> cpu_threads{};
        std::size_t cpu_thread_count = 0;
        std::thread::id cuda_submission{};
        std::thread::id xdma_worker{};
        for (std::size_t index = 0; index < operation_count; ++index) {
            const auto code = operations.codes[index];
            const auto thread = operations.threads[index];
            if (thread == std::thread::id{} || thread == control_thread) {
                return false;
            }
            if (code == 'P' || code == 'U' || code == 'B' || code == 'X' ||
                code == 'V') {
                bool found = false;
                for (std::size_t cpu = 0; cpu < cpu_thread_count; ++cpu) {
                    found = found || cpu_threads[cpu] == thread;
                }
                if (!found) {
                    if (cpu_thread_count >= cpu_threads.size()) {
                        return false;
                    }
                    cpu_threads[cpu_thread_count++] = thread;
                }
            } else if (code == 'H' || code == 'G' || code == 'D' ||
                       code == 'E') {
                if (cuda_submission == std::thread::id{}) {
                    cuda_submission = thread;
                } else if (cuda_submission != thread) {
                    return false;
                }
            } else if (code == 'h' || code == 'w' || code == 'e' ||
                       code == 'c') {
                if (xdma_worker == std::thread::id{}) {
                    xdma_worker = thread;
                } else if (xdma_worker != thread) {
                    return false;
                }
            }
        }
        if (cpu_thread_count == 0 || cuda_submission == std::thread::id{} ||
            xdma_worker == std::thread::id{} ||
            cuda_submission == xdma_worker) {
            return false;
        }
        for (std::size_t cpu = 0; cpu < cpu_thread_count; ++cpu) {
            if (cpu_threads[cpu] == cuda_submission ||
                cpu_threads[cpu] == xdma_worker) {
                return false;
            }
        }
        const auto query = cuda_driver.query_threads[0];
        if (query == std::thread::id{} || query == control_thread ||
            query == cuda_submission || query == xdma_worker) {
            return false;
        }
        for (std::size_t cpu = 0; cpu < cpu_thread_count; ++cpu) {
            if (query == cpu_threads[cpu]) {
                return false;
            }
        }
        return executor_instances == 2 && submission_instances == 2 &&
               service_instances == 1;
    }

    bool success_contract() noexcept {
        constexpr std::array expected{
            'P', 'U', 'H', 'G', 'D', 'E', 'B', 'X', 'h', 'w', 'e', 'c', 'V',
            'P', 'U', 'H', 'G', 'D', 'E', 'B', 'X', 'h', 'w', 'e', 'c', 'V'};
        if (!configuration_contract_valid || completed_frames != frame_count ||
            measured_allocations != 0 || !operations.matches(expected) ||
            !bridge_exact.load(std::memory_order_acquire) ||
            prepare_calls.load() != frame_count ||
            cuda_provider_calls.load() != frame_count ||
            bridge_calls.load() != frame_count ||
            xdma_provider_calls.load() != frame_count ||
            validate_calls.load() != frame_count ||
            cuda_driver.uploads.load() != frame_count ||
            cuda_driver.graph_launches.load() != frame_count ||
            cuda_driver.downloads.load() != frame_count ||
            cuda_driver.event_records.load() != frame_count ||
            xdma_driver.h2c_calls.load() != frame_count ||
            xdma_driver.control_writes.load() != frame_count ||
            xdma_driver.event_waits.load() != frame_count ||
            xdma_driver.c2h_calls.load() != frame_count ||
            !std::equal(xdma_stage.begin(), xdma_stage.end(),
                        expected_final.begin()) ||
            memory_plan.device_backend_count != 2 ||
            memory_plan.device_buffer_count != 2 ||
            memory_plan.device_batch_backend_count != 2 ||
            memory_plan.device_timeline_count != 2 ||
            !threads_are_bounded_and_separated() || !trace_causality(4)) {
            return false;
        }
        for (std::size_t index = 0; index < frame_count; ++index) {
            const auto value = static_cast<std::uint64_t>(index + 1);
            const auto& observation = timeline_observations[index];
            if (observation.cuda_accepted != value ||
                observation.cuda_completed != value ||
                observation.xdma_accepted != value ||
                observation.xdma_completed != value) {
                return false;
            }
        }
        return cuda_timeline.valid() && xdma_timeline.valid() &&
               cuda_timeline != xdma_timeline &&
               cuda_declaration.wait_count == 0 &&
               xdma_declaration.wait_count == 0 &&
               cuda_declaration.signals[0].timeline_handle ==
                   cuda_timeline.value &&
               xdma_declaration.signals[0].timeline_handle ==
                   xdma_timeline.value;
    }
};

} // namespace rtfw_combined_sample

#if !defined(RTFW_COMBINED_SAMPLE_NO_MAIN)
int main() {
    rtfw_combined_sample::Scenario scenario;
    auto status = scenario.configure();
    if (status == rt::Status::ok) {
        status = scenario.finalize();
    }
    if (status == rt::Status::ok) {
        status = scenario.start();
    }
    if (status == rt::Status::ok) {
        status = scenario.run(rtfw_combined_sample::frame_count);
    }
    const bool valid = status == rt::Status::ok &&
                       scenario.success_contract();
    const auto stop_status = scenario.stop();
    if (!valid || stop_status != rt::Status::ok) {
        std::cerr << "sample_cpu_gpu_fpga_cpu failed status="
                  << rt::status_message(status) << " stop="
                  << rt::status_message(stop_status) << '\n';
        return 1;
    }
    std::cout << "sample_cpu_gpu_fpga_cpu frames="
              << rtfw_combined_sample::frame_count
              << " evidence=simulated_protocol physical_hardware=false"
                 " direct_peer_dma=false\n";
    return 0;
}
#endif

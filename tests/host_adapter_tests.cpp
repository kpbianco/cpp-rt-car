#include <rt/c_api.h>
#include <rt/runtime.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>

namespace {

std::atomic<std::size_t> allocation_count{0};
std::atomic<bool> track_allocations{false};

template <class Job, std::size_t Capacity>
class FixedJobQueue {
public:
    bool push(const Job& job) noexcept {
        if (size_ == capacity_) {
            return false;
        }
        jobs_[tail_] = job;
        tail_ = (tail_ + 1) % Capacity;
        ++size_;
        return true;
    }

    bool pop(Job& job) noexcept {
        if (size_ == 0) {
            return false;
        }
        job = jobs_[head_];
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }

    void set_capacity(std::size_t capacity) noexcept {
        assert(capacity <= Capacity);
        capacity_ = capacity;
    }

private:
    std::array<Job, Capacity> jobs_{};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;
    std::size_t capacity_ = Capacity;
};

struct FakeHost {
    FixedJobQueue<rt::HostExecutorJob, 32> jobs;
    rt::HostExecutorJob retained{};
    std::byte* active_scratch = nullptr;
    std::size_t active_scratch_bytes = 0;
    std::size_t submitted = 0;
    std::size_t executed = 0;
    std::uint32_t next_worker = 0;
    std::uint32_t worker_count = 1;
    bool reject_all = false;

    static rt::Status submit(
        void* opaque,
        const rt::HostExecutorJob& job) noexcept {
        auto& self = *static_cast<FakeHost*>(opaque);
        if (self.reject_all || !self.jobs.push(job)) {
            return rt::Status::queue_full;
        }
        if (self.submitted == 0) {
            self.retained = job;
        }
        ++self.submitted;
        return rt::Status::ok;
    }

    static bool try_execute_one(void* opaque) noexcept {
        auto& self = *static_cast<FakeHost*>(opaque);
        rt::HostExecutorJob job;
        if (!self.jobs.pop(job)) {
            return false;
        }
        assert(job.execute != nullptr);
        const auto previous_scratch = self.active_scratch;
        const auto previous_scratch_bytes = self.active_scratch_bytes;
        self.active_scratch = job.scratch;
        self.active_scratch_bytes = job.scratch_bytes;
        const auto worker = self.next_worker++ % self.worker_count;
        job.execute(
            job.execution_context,
            job.completion_context,
            job.completion_token,
            worker);
        self.active_scratch = previous_scratch;
        self.active_scratch_bytes = previous_scratch_bytes;
        ++self.executed;
        return true;
    }
};

struct Workload {
    FakeHost* host = nullptr;
    std::size_t phase_calls = 0;
    std::size_t range_items = 0;
    std::size_t dependent_calls = 0;
};

struct ThreadedHost {
    static constexpr std::size_t capacity = 64;
    static constexpr std::size_t attempt_limit = 256;
    static constexpr std::uint32_t worker_count = 2;
    static constexpr std::uint32_t invalid_worker =
        std::numeric_limits<std::uint32_t>::max();

    struct Cell {
        std::atomic<std::size_t> sequence{0};
        rt::HostExecutorJob job{};
    };

    std::array<Cell, capacity> cells{};
    std::atomic<std::size_t> enqueue{0};
    std::atomic<std::size_t> dequeue{0};
    std::atomic<bool> stopping{false};
    std::array<std::thread, worker_count> workers;
    std::atomic<std::size_t> submitted{0};
    std::atomic<std::size_t> executed{0};
    inline static thread_local std::uint32_t active_worker =
        invalid_worker;

    ThreadedHost() {
        for (std::size_t index = 0; index < capacity; ++index) {
            cells[index].sequence.store(index, std::memory_order_relaxed);
        }
        for (std::uint32_t worker = 0; worker < worker_count; ++worker) {
            workers[worker] = std::thread([this, worker] {
                active_worker = worker;
                while (!stopping.load(std::memory_order_acquire)) {
                    if (!execute_one(worker)) {
                        std::this_thread::yield();
                    }
                }
                active_worker = invalid_worker;
            });
        }
    }

    ~ThreadedHost() {
        stopping.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }
    }

    ThreadedHost(const ThreadedHost&) = delete;
    ThreadedHost& operator=(const ThreadedHost&) = delete;

    bool push(const rt::HostExecutorJob& job) noexcept {
        auto position = enqueue.load(std::memory_order_relaxed);
        for (std::size_t attempt = 0;
             attempt < attempt_limit;
             ++attempt) {
            auto& cell = cells[position & (capacity - 1)];
            const auto sequence =
                cell.sequence.load(std::memory_order_acquire);
            const auto difference =
                static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (enqueue.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    cell.job = job;
                    cell.sequence.store(
                        position + 1,
                        std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue.load(std::memory_order_relaxed);
            }
        }
        return false;
    }

    bool pop(rt::HostExecutorJob& job) noexcept {
        auto position = dequeue.load(std::memory_order_relaxed);
        for (std::size_t attempt = 0;
             attempt < attempt_limit;
             ++attempt) {
            auto& cell = cells[position & (capacity - 1)];
            const auto sequence =
                cell.sequence.load(std::memory_order_acquire);
            const auto difference =
                static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position + 1);
            if (difference == 0) {
                if (dequeue.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    job = cell.job;
                    cell.sequence.store(
                        position + capacity,
                        std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = dequeue.load(std::memory_order_relaxed);
            }
        }
        return false;
    }

    bool execute_one(std::uint32_t worker) noexcept {
        rt::HostExecutorJob job;
        if (!pop(job)) {
            return false;
        }
        job.execute(
            job.execution_context,
            job.completion_context,
            job.completion_token,
            worker);
        executed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    static rt::Status submit(
        void* opaque,
        const rt::HostExecutorJob& job) noexcept {
        auto& self = *static_cast<ThreadedHost*>(opaque);
        if (!self.push(job)) {
            return rt::Status::queue_full;
        }
        self.submitted.fetch_add(1, std::memory_order_relaxed);
        return rt::Status::ok;
    }

    static bool try_execute_one(void* opaque) noexcept {
        auto& self = *static_cast<ThreadedHost*>(opaque);
        return active_worker < worker_count &&
            self.execute_one(active_worker);
    }
};

struct ThreadedWorkload {
    std::atomic<std::size_t> roots{0};
    std::atomic<std::size_t> range_items{0};
    std::atomic<std::size_t> dependents{0};
};

rt::TaskResult count_threaded_range(
    void* opaque,
    const rt::TaskContext&,
    const rt::TaskRange& range) {
    static_cast<ThreadedWorkload*>(opaque)->range_items.fetch_add(
        range.end - range.begin,
        std::memory_order_relaxed);
    return rt::TaskResult::ok;
}

rt::CallbackResult run_threaded_root(
    void* opaque,
    const rt::CallbackContext& context) {
    auto& workload = *static_cast<ThreadedWorkload*>(opaque);
    workload.roots.fetch_add(1, std::memory_order_relaxed);
    return context.tasks.parallel_for(
               256,
               4,
               &count_threaded_range,
               &workload) == rt::Status::ok
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

rt::CallbackResult run_threaded_dependent(
    void* opaque,
    const rt::CallbackContext&) {
    static_cast<ThreadedWorkload*>(opaque)->dependents.fetch_add(
        1,
        std::memory_order_relaxed);
    return rt::CallbackResult::ok;
}

rt::TaskResult count_range(
    void* opaque,
    const rt::TaskContext& context,
    const rt::TaskRange& range) {
    auto& workload = *static_cast<Workload*>(opaque);
    assert(context.scratch().size() == 64);
    assert(context.scratch().data() != nullptr);
    workload.range_items += range.end - range.begin;
    return rt::TaskResult::ok;
}

rt::CallbackResult run_root(
    void* opaque,
    const rt::CallbackContext& context) {
    auto& workload = *static_cast<Workload*>(opaque);
    ++workload.phase_calls;
    const auto task_scratch = context.tasks.scratch();
    assert(task_scratch.data() == workload.host->active_scratch);
    assert(task_scratch.size() == workload.host->active_scratch_bytes);
    return context.tasks.parallel_for(
               32,
               4,
               &count_range,
               &workload) == rt::Status::ok
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

rt::CallbackResult run_dependent(
    void* opaque,
    const rt::CallbackContext&) {
    ++static_cast<Workload*>(opaque)->dependent_calls;
    return rt::CallbackResult::ok;
}

void cpp_host_adapter_contract() {
    rt::Runtime missing;
    rt::RuntimeConfig missing_config;
    missing_config.executor_policy = rt::ExecutorPolicy::host_adapter;
    assert(missing.configure(missing_config) == rt::Status::ok);
    assert(missing.finalize() == rt::Status::invalid_config);

    FakeHost host;
    host.worker_count = 2;
    host.jobs.set_capacity(8);
    rt::HostExecutorAdapter adapter{
        &host,
        2,
        8,
        &FakeHost::submit,
        &FakeHost::try_execute_one,
    };

    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.executor_policy = rt::ExecutorPolicy::host_adapter;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_bytes = 64;
    config.task_scratch_slots = 32;
    assert(runtime.configure(config) == rt::Status::ok);
    assert(runtime.set_host_executor(adapter) == rt::Status::ok);

    Workload workload{&host};
    rt::PhaseHandle root;
    rt::PhaseHandle dependent;
    assert(runtime.register_callback(
               {"root", &run_root, &workload},
               root) == rt::Status::ok);
    assert(runtime.register_callback(
               {"dependent", &run_dependent, &workload},
               dependent) == rt::Status::ok);
    assert(runtime.add_dependency(root, dependent) == rt::Status::ok);
    assert(runtime.finalize() == rt::Status::ok);
    rt::MemoryPlan plan;
    assert(runtime.memory_plan(plan));
    assert(plan.queue_slots == 8);
    assert(runtime.start() == rt::Status::ok);

    allocation_count.store(0, std::memory_order_relaxed);
    track_allocations.store(true, std::memory_order_release);
    rt::StepResult result;
    const auto first_status = runtime.step(
        {1, std::chrono::nanoseconds(1'000'000), std::nullopt},
        &result);
    track_allocations.store(false, std::memory_order_release);
    assert(first_status == rt::Status::ok);
    assert(allocation_count.load(std::memory_order_acquire) == 0);
    assert(result.callbacks_executed == 2);
    assert(workload.phase_calls == 1);
    assert(workload.range_items == 32);
    assert(workload.dependent_calls == 1);
    assert(host.submitted == 10);
    assert(host.executed == 10);

    const auto stats = runtime.executor_stats();
    assert(stats.policy == rt::ExecutorPolicy::host_adapter);
    assert(stats.worker_count == 2);
    assert(stats.worker_starts == 0);
    assert(stats.submitted_tasks == 10);

    assert(runtime.step(
               {2, std::chrono::nanoseconds(1'000'000), std::nullopt}) ==
           rt::Status::ok);
    const auto calls_before_stale = workload.phase_calls;
    host.retained.execute(
        host.retained.execution_context,
        host.retained.completion_context,
        host.retained.completion_token,
        0);
    assert(workload.phase_calls == calls_before_stale);
    assert(runtime.stop() == rt::Status::ok);

    FakeHost saturated;
    saturated.worker_count = 1;
    saturated.reject_all = true;
    rt::Runtime overloaded;
    rt::RuntimeConfig overload_config;
    overload_config.executor_policy = rt::ExecutorPolicy::host_adapter;
    overload_config.worker_count = 1;
    overload_config.executor_queue_capacity = 8;
    overload_config.task_scratch_slots = 8;
    assert(overloaded.configure(overload_config) == rt::Status::ok);
    assert(overloaded.set_host_executor({
               &saturated,
               1,
               8,
               &FakeHost::submit,
               &FakeHost::try_execute_one}) == rt::Status::ok);
    assert(overloaded.register_callback(
               {"root", &run_dependent, &workload}) == rt::Status::ok);
    assert(overloaded.finalize() == rt::Status::ok);
    assert(overloaded.start() == rt::Status::ok);
    assert(overloaded.step(
               {3, std::chrono::nanoseconds(1), std::nullopt}) ==
           rt::Status::queue_full);
    assert(overloaded.stop() == rt::Status::ok);
}

void threaded_host_adapter_contract() {
    ThreadedHost host;
    ThreadedWorkload workload;
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.executor_policy = rt::ExecutorPolicy::host_adapter;
    config.worker_count = ThreadedHost::worker_count;
    config.executor_queue_capacity = ThreadedHost::capacity;
    config.task_scratch_bytes = 64;
    config.task_scratch_slots = 128;
    assert(runtime.configure(config) == rt::Status::ok);
    assert(runtime.set_host_executor({
               &host,
               ThreadedHost::worker_count,
               ThreadedHost::capacity,
               &ThreadedHost::submit,
               &ThreadedHost::try_execute_one}) == rt::Status::ok);

    rt::PhaseHandle root;
    rt::PhaseHandle dependent;
    assert(runtime.register_callback(
               {"threaded.root", &run_threaded_root, &workload},
               root) == rt::Status::ok);
    assert(runtime.register_callback(
               {"threaded.dependent", &run_threaded_dependent, &workload},
               dependent) == rt::Status::ok);
    assert(runtime.add_dependency(root, dependent) == rt::Status::ok);
    assert(runtime.finalize() == rt::Status::ok);
    assert(runtime.start() == rt::Status::ok);

    allocation_count.store(0, std::memory_order_relaxed);
    track_allocations.store(true, std::memory_order_release);
    const auto status = runtime.step(
        {7, std::chrono::nanoseconds(1'000'000), std::nullopt});
    track_allocations.store(false, std::memory_order_release);
    assert(status == rt::Status::ok);
    assert(allocation_count.load(std::memory_order_acquire) == 0);
    assert(workload.roots.load(std::memory_order_acquire) == 1);
    assert(workload.range_items.load(std::memory_order_acquire) == 256);
    assert(workload.dependents.load(std::memory_order_acquire) == 1);
    assert(host.submitted.load(std::memory_order_acquire) == 66);
    for (std::size_t attempt = 0;
         host.executed.load(std::memory_order_acquire) != 66 &&
             attempt < 100'000;
         ++attempt) {
        std::this_thread::yield();
    }
    assert(host.executed.load(std::memory_order_acquire) == 66);
    const auto stats = runtime.executor_stats();
    assert(stats.worker_starts == 0);
    assert(stats.local_executions == 66);
    assert(runtime.stop() == rt::Status::ok);
}

struct CFakeHost {
    FixedJobQueue<rtfw_host_job, 16> jobs;
    std::uint32_t next_worker = 0;
    std::size_t callback_count = 0;
    std::size_t range_items = 0;
};

rtfw_status c_submit(
    void* opaque,
    const rtfw_host_job* job) {
    auto& host = *static_cast<CFakeHost*>(opaque);
    if (!job || job->struct_size < sizeof(*job) ||
        job->reserved0 != 0 || !host.jobs.push(*job)) {
        return RTFW_STATUS_QUEUE_FULL;
    }
    return RTFW_STATUS_OK;
}

uint8_t c_try_execute_one(void* opaque) {
    auto& host = *static_cast<CFakeHost*>(opaque);
    rtfw_host_job job{};
    if (!host.jobs.pop(job)) {
        return 0;
    }
    job.execute(
        job.execution_context,
        job.completion_context,
        job.completion_token,
        host.next_worker++ % 2u);
    return 1;
}

rtfw_callback_result c_count_range(
    void* opaque,
    const rtfw_task_context*,
    uint64_t begin,
    uint64_t end,
    uint64_t) {
    auto& host = *static_cast<CFakeHost*>(opaque);
    host.range_items += static_cast<std::size_t>(end - begin);
    return RTFW_CALLBACK_OK;
}

rtfw_callback_result c_root(
    void* opaque,
    const rtfw_callback_context* context) {
    auto& host = *static_cast<CFakeHost*>(opaque);
    ++host.callback_count;
    return rtfw_parallel_for(
               context->tasks,
               16,
               4,
               &c_count_range,
               &host) == RTFW_STATUS_OK
        ? RTFW_CALLBACK_OK
        : RTFW_CALLBACK_ERROR;
}

void c_host_adapter_contract() {
    assert(rtfw_check_abi(
               RTFW_C_ABI_VERSION,
               RTFW_C_ABI_LAYOUT_FINGERPRINT) == RTFW_STATUS_OK);
    assert(rtfw_check_abi(
               RTFW_C_ABI_VERSION - 1,
               RTFW_C_ABI_LAYOUT_FINGERPRINT) ==
           RTFW_STATUS_INCOMPATIBLE_ABI);
    rtfw_abi_info info;
    rtfw_abi_info_init(&info);
    assert(rtfw_get_abi_info(&info) == RTFW_STATUS_OK);
    assert(info.abi_version == RTFW_C_ABI_VERSION);
    assert(info.layout_fingerprint ==
           RTFW_C_ABI_LAYOUT_FINGERPRINT);
    assert((info.feature_flags &
            RTFW_ABI_FEATURE_HOST_EXECUTOR_ADAPTER) != 0);

    rtfw_config config;
    rtfw_config_init(&config);
    config.executor_policy = RTFW_EXECUTOR_HOST_ADAPTER;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_bytes = 64;
    config.task_scratch_slots = 16;

    rtfw_handle* runtime = nullptr;
    assert(rtfw_create(&config, &runtime) == RTFW_STATUS_OK);
    assert(runtime != nullptr);
    CFakeHost host;
    host.jobs.set_capacity(8);
    rtfw_host_executor adapter;
    rtfw_host_executor_init(&adapter);
    adapter.user_data = &host;
    adapter.worker_count = 2;
    adapter.queue_capacity = 8;
    adapter.submit = &c_submit;
    adapter.try_execute_one = &c_try_execute_one;
    assert(rtfw_set_host_executor(runtime, &adapter) ==
           RTFW_STATUS_OK);
    assert(rtfw_register_callback(
               runtime,
               "c.root",
               &c_root,
               &host) == RTFW_STATUS_OK);
    assert(rtfw_finalize(runtime) == RTFW_STATUS_OK);
    assert(rtfw_start(runtime) == RTFW_STATUS_OK);

    rtfw_frame_context frame;
    rtfw_frame_context_init(&frame);
    frame.frame_index = 1;
    frame.delta_ns = 1'000'000;
    assert(rtfw_step(runtime, &frame, nullptr) == RTFW_STATUS_OK);
    assert(host.callback_count == 1);
    assert(host.range_items == 16);
    assert(rtfw_stop(runtime) == RTFW_STATUS_OK);
    rtfw_destroy(runtime);
}

} // namespace

void* operator new(std::size_t size) {
    if (track_allocations.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* storage = std::malloc(size == 0 ? 1 : size)) {
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
    cpp_host_adapter_contract();
    threaded_host_adapter_contract();
    c_host_adapter_contract();
    return 0;
}

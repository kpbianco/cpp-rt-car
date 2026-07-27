#include <rt/runtime.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace {

struct HostQueue {
    std::array<rt::HostExecutorJob, 8> jobs{};
    std::size_t head = 0;
    std::size_t tail = 0;
    std::size_t count = 0;
    std::uint32_t next_worker = 0;

    static rt::Status submit(
        void* opaque,
        const rt::HostExecutorJob& job) noexcept {
        auto& self = *static_cast<HostQueue*>(opaque);
        if (self.count == self.jobs.size()) {
            return rt::Status::queue_full;
        }
        self.jobs[self.tail] = job;
        self.tail = (self.tail + 1) % self.jobs.size();
        ++self.count;
        return rt::Status::ok;
    }

    static bool try_execute_one(void* opaque) noexcept {
        auto& self = *static_cast<HostQueue*>(opaque);
        if (self.count == 0) {
            return false;
        }
        const auto job = self.jobs[self.head];
        self.head = (self.head + 1) % self.jobs.size();
        --self.count;
        job.execute(
            job.execution_context,
            job.completion_context,
            job.completion_token,
            self.next_worker++ % 2u);
        return true;
    }
};

rt::CallbackResult phase(void* opaque, const rt::CallbackContext&) {
    ++*static_cast<std::size_t*>(opaque);
    return rt::CallbackResult::ok;
}

} // namespace

int main() {
    HostQueue queue;
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.executor_policy = rt::ExecutorPolicy::host_adapter;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    std::size_t calls = 0;

    if (runtime.configure(config) != rt::Status::ok ||
        runtime.set_host_executor({
            &queue,
            2,
            8,
            &HostQueue::submit,
            &HostQueue::try_execute_one}) != rt::Status::ok ||
        runtime.register_callback({"consumer.phase", &phase, &calls}) !=
            rt::Status::ok ||
        runtime.finalize() != rt::Status::ok ||
        runtime.start() != rt::Status::ok ||
        runtime.step({
            1,
            std::chrono::nanoseconds(1'000'000),
            std::nullopt}) != rt::Status::ok ||
        runtime.stop() != rt::Status::ok) {
        return 1;
    }
    return calls == 1 ? 0 : 2;
}

#pragma once

#include "aligned_storage.hpp"
#include "compiled_graph.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include <rt/runtime.hpp>

namespace rt::detail {

class ThreadPolicyTransaction;

struct PhaseTaskDispatch {
    Status status = Status::callback_failed;
    bool pending = false;
};

using PhaseTaskCallback = PhaseTaskDispatch (*)(
    void* user_data,
    std::uint32_t phase_index,
    const TaskContext& task_context);

class Executor final {
public:
    Executor(
        ExecutorPolicy policy,
        std::size_t worker_count,
        std::size_t queue_capacity,
        std::size_t phase_count,
        std::size_t task_scratch_bytes,
        std::size_t task_scratch_slots,
        std::size_t scratch_alignment,
        OverloadPolicy overload_policy,
        std::span<const GraphDependency> dependencies,
        const HostExecutorAdapter* host_adapter);
    ~Executor();

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    [[nodiscard]] Status start(
        ThreadPolicyTransaction* transaction = nullptr) noexcept;
    void stop() noexcept;

    [[nodiscard]] Status run(
        PhaseTaskCallback callback,
        void* user_data,
        std::size_t& callbacks_executed,
        std::size_t& failed_phase) noexcept;
    // Called only by the runtime-owned device completion lane. It never
    // executes host code and releases graph successors only after the phase's
    // submission callback has returned.
    [[nodiscard]] Status complete_external(
        std::size_t phase_index,
        Status status) noexcept;

    [[nodiscard]] Status parallel_for(
        const TaskContext& parent,
        std::size_t item_count,
        std::size_t grain_size,
        RangeTaskCallback callback,
        void* user_data) noexcept;

    [[nodiscard]] Status parallel_reduce(
        const TaskContext& parent,
        std::size_t item_count,
        std::size_t grain_size,
        RangeTaskCallback range_callback,
        ReductionTaskCallback combine_callback,
        void* user_data) noexcept;

    [[nodiscard]] ExecutorStats stats() const noexcept;
    [[nodiscard]] bool static_assignment(
        std::size_t phase_index,
        std::size_t& worker_index) const noexcept;
    [[nodiscard]] static bool estimate_control_storage(
        ExecutorPolicy policy,
        std::size_t worker_count,
        std::size_t queue_capacity,
        std::size_t phase_count,
        std::size_t dependency_count,
        std::size_t task_scratch_slots,
        std::size_t& bytes) noexcept;

private:
    enum class WorkKind : std::uint8_t {
        phase,
        range,
        reduction,
    };

    struct TaskGroup {
        std::atomic<std::size_t> pending{0};
        std::atomic<std::int32_t> status{
            static_cast<std::int32_t>(Status::ok)};

        void reset() noexcept;
        void record(Status value) noexcept;
        [[nodiscard]] Status result() const noexcept;
    };

    struct WorkItem {
        WorkKind kind = WorkKind::phase;
        TaskGroup* group = nullptr;
        RangeTaskCallback range_callback = nullptr;
        ReductionTaskCallback reduction_callback = nullptr;
        void* user_data = nullptr;
        std::uint32_t phase_index = 0;
        std::size_t task_index = 0;
        std::size_t range_begin = 0;
        std::size_t range_end = 0;
        std::size_t peer_task_index = 0;
        std::size_t scratch_slot = static_cast<std::size_t>(-1);
    };

    struct HostWorkSlot {
        // The low two bits are state; the remaining bits are the generation.
        // One CAS therefore validates both before an external callback runs.
        std::atomic<std::uint64_t> control{0};
        WorkItem item{};
    };

    class Queue;

    [[nodiscard]] Status submit(
        WorkItem item,
        std::size_t target_worker) noexcept;
    [[nodiscard]] Status submit_phase(std::uint32_t phase_index) noexcept;
    [[nodiscard]] bool acquire_scratch_slot(
        std::size_t& scratch_slot) noexcept;
    void release_scratch_slot(std::size_t scratch_slot) noexcept;
    [[nodiscard]] Status reject_overload(
        Status status,
        std::size_t phase_index) noexcept;
    [[nodiscard]] Status wait(
        TaskGroup& group,
        std::size_t helping_worker) noexcept;
    [[nodiscard]] bool execute_one(std::size_t worker_index) noexcept;
    void execute(std::size_t worker_index, const WorkItem& item) noexcept;
    static void execute_host_job(
        void* execution_context,
        void* completion_context,
        std::uint64_t completion_token,
        std::uint32_t worker_index) noexcept;
    void execute_host_slot(
        HostWorkSlot& slot,
        std::uint64_t completion_token,
        std::size_t worker_index) noexcept;
    void execute_phase(
        std::size_t worker_index,
        const WorkItem& item,
        const TaskContext& context) noexcept;
    void finish_phase(
        std::size_t phase_index,
        Status status) noexcept;
    void worker_loop(std::size_t worker_index) noexcept;
    void cancel_graph(Status status, std::size_t failed_phase) noexcept;
    [[nodiscard]] std::size_t static_worker(
        std::size_t phase_index,
        std::size_t task_index) const noexcept;

    ExecutorPolicy policy_;
    std::size_t worker_count_;
    std::size_t queue_capacity_;
    std::size_t phase_count_;
    std::size_t task_scratch_bytes_;
    std::size_t task_scratch_stride_;
    std::size_t task_scratch_slots_;
    std::size_t scratch_alignment_;
    OverloadPolicy overload_policy_;
    std::vector<std::unique_ptr<Queue>> queues_;
    std::vector<std::thread> threads_;
    HostExecutorAdapter host_adapter_{};
    std::unique_ptr<HostWorkSlot[]> host_work_slots_;
    AlignedStorage task_scratch_storage_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> free_scratch_words_;
    std::size_t free_scratch_word_count_ = 0;
    std::atomic<std::size_t> scratch_word_hint_{0};

    std::vector<std::uint32_t> initial_indegree_;
    std::unique_ptr<std::atomic<std::uint32_t>[]> current_indegree_;
    std::unique_ptr<std::atomic<std::uint8_t>[]> phase_states_;
    std::unique_ptr<std::atomic<std::int32_t>[]> phase_statuses_;
    std::vector<std::size_t> successor_offsets_;
    std::vector<std::uint32_t> successors_;
    std::vector<std::size_t> static_assignments_;

    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    TaskGroup graph_group_{};
    std::atomic<bool> graph_cancelled_{false};
    std::atomic<std::size_t> graph_callbacks_executed_{0};
    std::atomic<std::size_t> graph_failed_phase_{
        static_cast<std::size_t>(-1)};
    PhaseTaskCallback phase_callback_ = nullptr;
    void* phase_user_data_ = nullptr;

    std::atomic<std::uint64_t> submitted_tasks_{0};
    std::atomic<std::uint64_t> local_executions_{0};
    std::atomic<std::uint64_t> steal_attempts_{0};
    std::atomic<std::uint64_t> successful_steals_{0};
    std::atomic<std::uint64_t> queue_full_rejections_{0};
    std::atomic<std::uint64_t> scratch_exhaustions_{0};
    std::atomic<std::uint64_t> worker_starts_{0};
    std::atomic<std::uint64_t> host_completion_sequence_{4};
    ThreadPolicyTransaction* startup_transaction_ = nullptr;
};

} // namespace rt::detail

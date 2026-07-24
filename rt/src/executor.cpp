#include "executor.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#include <rt/arch.hpp>
#include <rt/numerics.hpp>

namespace {

constexpr std::size_t kInvalidWorker = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kQueueCasAttemptLimit = 64;

} // namespace

namespace rt::detail {

class Executor::Queue final {
public:
    explicit Queue(std::size_t capacity)
        : cells_(std::make_unique<Cell[]>(capacity)),
          capacity_(capacity),
          mask_(capacity - 1) {
        for (std::size_t index = 0; index < capacity_; ++index) {
            cells_[index].sequence.store(index, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool try_push(const WorkItem& item) noexcept {
        std::size_t position = enqueue_.load(std::memory_order_relaxed);
        for (std::size_t attempt = 0;
             attempt < kQueueCasAttemptLimit;
             ++attempt) {
            Cell& cell = cells_[position & mask_];
            const auto sequence =
                cell.sequence.load(std::memory_order_acquire);
            const auto difference =
                static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (enqueue_.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    cell.item = item;
                    cell.sequence.store(
                        position + 1,
                        std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_.load(std::memory_order_relaxed);
            }
            rt::cpu_relax();
        }
        return false;
    }

    [[nodiscard]] bool try_pop(WorkItem& item) noexcept {
        std::size_t position = dequeue_.load(std::memory_order_relaxed);
        for (std::size_t attempt = 0;
             attempt < kQueueCasAttemptLimit;
             ++attempt) {
            Cell& cell = cells_[position & mask_];
            const auto sequence =
                cell.sequence.load(std::memory_order_acquire);
            const auto difference =
                static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position + 1);
            if (difference == 0) {
                if (dequeue_.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    item = cell.item;
                    cell.sequence.store(
                        position + capacity_,
                        std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = dequeue_.load(std::memory_order_relaxed);
            }
            rt::cpu_relax();
        }
        return false;
    }

private:
    struct Cell {
        std::atomic<std::size_t> sequence{0};
        WorkItem item{};
    };

    std::unique_ptr<Cell[]> cells_;
    std::size_t capacity_;
    std::size_t mask_;
    alignas(64) std::atomic<std::size_t> enqueue_{0};
    alignas(64) std::atomic<std::size_t> dequeue_{0};
};

void Executor::TaskGroup::reset() noexcept {
    pending.store(0, std::memory_order_relaxed);
    status.store(
        static_cast<std::int32_t>(Status::ok),
        std::memory_order_relaxed);
}

void Executor::TaskGroup::record(Status value) noexcept {
    if (value == Status::ok) {
        return;
    }
    auto expected = static_cast<std::int32_t>(Status::ok);
    status.compare_exchange_strong(
        expected,
        static_cast<std::int32_t>(value),
        std::memory_order_acq_rel,
        std::memory_order_relaxed);
}

Status Executor::TaskGroup::result() const noexcept {
    return static_cast<Status>(status.load(std::memory_order_acquire));
}

Executor::Executor(
    ExecutorPolicy policy,
    std::size_t worker_count,
    std::size_t queue_capacity,
    std::size_t phase_count,
    std::span<const GraphDependency> dependencies)
    : policy_(policy),
      worker_count_(worker_count),
      queue_capacity_(queue_capacity),
      phase_count_(phase_count),
      initial_indegree_(phase_count, 0),
      current_indegree_(
          phase_count == 0
              ? nullptr
              : std::make_unique<std::atomic<std::uint32_t>[]>(phase_count)),
      successor_offsets_(phase_count + 1, 0),
      static_assignments_(phase_count, 0) {
    queues_.reserve(worker_count_);
    threads_.reserve(worker_count_);
    for (std::size_t worker = 0; worker < worker_count_; ++worker) {
        queues_.push_back(std::make_unique<Queue>(queue_capacity_));
    }

    for (const auto& dependency : dependencies) {
        const auto prerequisite =
            static_cast<std::size_t>(dependency.prerequisite.index());
        const auto dependent =
            static_cast<std::size_t>(dependency.dependent.index());
        ++initial_indegree_[dependent];
        ++successor_offsets_[prerequisite + 1];
    }
    for (std::size_t phase = 1; phase < successor_offsets_.size(); ++phase) {
        successor_offsets_[phase] += successor_offsets_[phase - 1];
    }

    successors_.resize(dependencies.size());
    auto cursors = successor_offsets_;
    for (const auto& dependency : dependencies) {
        const auto prerequisite =
            static_cast<std::size_t>(dependency.prerequisite.index());
        successors_[cursors[prerequisite]++] =
            dependency.dependent.index();
    }

    for (std::size_t phase = 0; phase < phase_count_; ++phase) {
        static_assignments_[phase] = phase % worker_count_;
    }
}

Executor::~Executor() {
    stop();
}

Status Executor::start() noexcept {
    if (started_.load(std::memory_order_acquire)) {
        return Status::invalid_state;
    }

    stopping_.store(false, std::memory_order_release);
    submitted_tasks_.store(0, std::memory_order_relaxed);
    local_executions_.store(0, std::memory_order_relaxed);
    steal_attempts_.store(0, std::memory_order_relaxed);
    successful_steals_.store(0, std::memory_order_relaxed);
    queue_full_rejections_.store(0, std::memory_order_relaxed);
    worker_starts_.store(0, std::memory_order_relaxed);
    started_.store(true, std::memory_order_release);

    try {
        for (std::size_t worker = 0; worker < worker_count_; ++worker) {
            threads_.emplace_back([this, worker] {
                worker_loop(worker);
            });
        }
    } catch (...) {
        stopping_.store(true, std::memory_order_release);
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads_.clear();
        started_.store(false, std::memory_order_release);
        return Status::resource_exhausted;
    }

    while (worker_starts_.load(std::memory_order_acquire) < worker_count_) {
        std::this_thread::yield();
    }
    return Status::ok;
}

void Executor::stop() noexcept {
    if (!started_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    stopping_.store(true, std::memory_order_release);
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
}

Status Executor::submit(
    WorkItem item,
    std::size_t target_worker) noexcept {
    if (!started_.load(std::memory_order_acquire) ||
        target_worker >= worker_count_ ||
        item.group == nullptr) {
        return Status::invalid_state;
    }

    item.group->pending.fetch_add(1, std::memory_order_acq_rel);
    if (!queues_[target_worker]->try_push(item)) {
        item.group->pending.fetch_sub(1, std::memory_order_acq_rel);
        queue_full_rejections_.fetch_add(1, std::memory_order_relaxed);
        return Status::queue_full;
    }
    submitted_tasks_.fetch_add(1, std::memory_order_relaxed);
    return Status::ok;
}

Status Executor::submit_phase(std::uint32_t phase_index) noexcept {
    WorkItem item;
    item.kind = WorkKind::phase;
    item.group = &graph_group_;
    item.phase_index = phase_index;
    return submit(item, static_assignments_[phase_index]);
}

void Executor::cancel_graph(
    Status status,
    std::size_t failed_phase) noexcept {
    graph_group_.record(status);
    graph_cancelled_.store(true, std::memory_order_release);
    if (failed_phase >= phase_count_) {
        return;
    }

    auto observed = graph_failed_phase_.load(std::memory_order_relaxed);
    while (failed_phase < observed &&
           !graph_failed_phase_.compare_exchange_weak(
               observed,
               failed_phase,
               std::memory_order_release,
               std::memory_order_relaxed)) {
    }
}

Status Executor::run(
    PhaseTaskCallback callback,
    void* user_data,
    std::size_t& callbacks_executed,
    std::size_t& failed_phase) noexcept {
    callbacks_executed = 0;
    failed_phase = phase_count_;
    if (!started_.load(std::memory_order_acquire) || callback == nullptr) {
        return Status::invalid_state;
    }

    graph_group_.reset();
    graph_cancelled_.store(false, std::memory_order_relaxed);
    graph_callbacks_executed_.store(0, std::memory_order_relaxed);
    graph_failed_phase_.store(phase_count_, std::memory_order_relaxed);
    phase_callback_ = callback;
    phase_user_data_ = user_data;
    for (std::size_t phase = 0; phase < phase_count_; ++phase) {
        current_indegree_[phase].store(
            initial_indegree_[phase],
            std::memory_order_relaxed);
    }

    for (std::uint32_t phase = 0; phase < phase_count_; ++phase) {
        if (initial_indegree_[phase] != 0) {
            continue;
        }
        const auto status = submit_phase(phase);
        if (status != Status::ok) {
            cancel_graph(status, phase_count_);
            break;
        }
    }

    const auto status = wait(graph_group_, kInvalidWorker);
    callbacks_executed =
        graph_callbacks_executed_.load(std::memory_order_acquire);
    failed_phase = graph_failed_phase_.load(std::memory_order_acquire);
    phase_callback_ = nullptr;
    phase_user_data_ = nullptr;
    return status;
}

Status Executor::parallel_for(
    const TaskContext& parent,
    std::size_t item_count,
    std::size_t grain_size,
    RangeTaskCallback callback,
    void* user_data) noexcept {
    if (parent.executor_ != this || callback == nullptr || grain_size == 0) {
        return Status::invalid_argument;
    }
    if (item_count == 0) {
        return Status::ok;
    }

    TaskGroup group;
    Status submission_status = Status::ok;
    const auto task_count = 1 + ((item_count - 1) / grain_size);
    for (std::size_t task_index = 0;
         task_index < task_count;
         ++task_index) {
        const auto begin = task_index * grain_size;
        const auto remaining = item_count - begin;

        WorkItem item;
        item.kind = WorkKind::range;
        item.group = &group;
        item.range_callback = callback;
        item.user_data = user_data;
        item.phase_index = static_cast<std::uint32_t>(parent.phase_index_);
        item.task_index = task_index;
        item.range_begin = begin;
        item.range_end = begin + std::min(grain_size, remaining);

        const auto target = policy_ == ExecutorPolicy::static_deterministic
            ? static_worker(parent.phase_index_, task_index)
            : parent.worker_index_;
        submission_status = submit(item, target);
        if (submission_status != Status::ok) {
            break;
        }
    }

    const auto task_status = wait(group, parent.worker_index_);
    return submission_status != Status::ok ? submission_status : task_status;
}

Status Executor::parallel_reduce(
    const TaskContext& parent,
    std::size_t item_count,
    std::size_t grain_size,
    RangeTaskCallback range_callback,
    ReductionTaskCallback combine_callback,
    void* user_data) noexcept {
    if (parent.executor_ != this || range_callback == nullptr ||
        combine_callback == nullptr || grain_size == 0) {
        return Status::invalid_argument;
    }
    if (item_count == 0) {
        return Status::ok;
    }

    const auto range_status = parallel_for(
        parent,
        item_count,
        grain_size,
        range_callback,
        user_data);
    if (range_status != Status::ok) {
        return range_status;
    }

    const auto task_count = 1 + ((item_count - 1) / grain_size);
    for (std::size_t stride = 1; stride < task_count;) {
        TaskGroup group;
        Status submission_status = Status::ok;
        for (std::size_t left = 0; left < task_count;) {
            if (stride > task_count - left) {
                break;
            }
            const auto right = left + stride;
            if (right >= task_count) {
                break;
            }

            WorkItem item;
            item.kind = WorkKind::reduction;
            item.group = &group;
            item.reduction_callback = combine_callback;
            item.user_data = user_data;
            item.phase_index =
                static_cast<std::uint32_t>(parent.phase_index_);
            item.task_index = left;
            item.peer_task_index = right;

            const auto target =
                policy_ == ExecutorPolicy::static_deterministic
                ? static_worker(parent.phase_index_, left)
                : parent.worker_index_;
            submission_status = submit(item, target);
            if (submission_status != Status::ok) {
                break;
            }
            const auto remaining = task_count - left;
            if (stride > remaining / 2) {
                break;
            }
            left += stride * 2;
        }

        const auto task_status = wait(group, parent.worker_index_);
        if (submission_status != Status::ok) {
            return submission_status;
        }
        if (task_status != Status::ok) {
            return task_status;
        }
        if (stride > task_count / 2) {
            break;
        }
        stride *= 2;
    }
    return Status::ok;
}

Status Executor::wait(
    TaskGroup& group,
    std::size_t helping_worker) noexcept {
    while (group.pending.load(std::memory_order_acquire) != 0) {
        if (helping_worker < worker_count_ &&
            execute_one(helping_worker)) {
            continue;
        }
        std::this_thread::yield();
    }
    return group.result();
}

bool Executor::execute_one(std::size_t worker_index) noexcept {
    WorkItem item;
    if (queues_[worker_index]->try_pop(item)) {
        local_executions_.fetch_add(1, std::memory_order_relaxed);
        execute(worker_index, item);
        return true;
    }

    if (policy_ != ExecutorPolicy::bounded_throughput) {
        return false;
    }

    for (std::size_t offset = 1; offset < worker_count_; ++offset) {
        const auto victim = (worker_index + offset) % worker_count_;
        steal_attempts_.fetch_add(1, std::memory_order_relaxed);
        if (queues_[victim]->try_pop(item)) {
            successful_steals_.fetch_add(1, std::memory_order_relaxed);
            execute(worker_index, item);
            return true;
        }
    }
    return false;
}

void Executor::execute(
    std::size_t worker_index,
    const WorkItem& item) noexcept {
    TaskContext context(
        *this,
        worker_index,
        item.phase_index,
        item.task_index);
    Status status = Status::ok;
    try {
        switch (item.kind) {
        case WorkKind::phase:
            execute_phase(worker_index, item, context);
            break;
        case WorkKind::range:
            if (item.range_callback(
                    item.user_data,
                    context,
                    TaskRange{
                        item.range_begin,
                        item.range_end,
                        item.task_index}) != TaskResult::ok) {
                status = Status::callback_failed;
            }
            break;
        case WorkKind::reduction:
            if (item.reduction_callback(
                    item.user_data,
                    context,
                    item.task_index,
                    item.peer_task_index) != TaskResult::ok) {
                status = Status::callback_failed;
            }
            break;
        }
    } catch (...) {
        status = Status::callback_failed;
    }

    item.group->record(status);
    item.group->pending.fetch_sub(1, std::memory_order_acq_rel);
}

void Executor::execute_phase(
    std::size_t,
    const WorkItem& item,
    const TaskContext& context) noexcept {
    if (graph_cancelled_.load(std::memory_order_acquire)) {
        return;
    }

    CallbackResult result = CallbackResult::error;
    try {
        result = phase_callback_(
            phase_user_data_,
            item.phase_index,
            context);
    } catch (...) {
        result = CallbackResult::error;
    }
    graph_callbacks_executed_.fetch_add(1, std::memory_order_relaxed);

    if (result != CallbackResult::ok) {
        cancel_graph(Status::callback_failed, item.phase_index);
        return;
    }
    if (graph_cancelled_.load(std::memory_order_acquire)) {
        return;
    }

    const auto begin = successor_offsets_[item.phase_index];
    const auto end = successor_offsets_[item.phase_index + 1];
    for (std::size_t cursor = begin; cursor < end; ++cursor) {
        const auto successor = successors_[cursor];
        if (current_indegree_[successor].fetch_sub(
                1,
                std::memory_order_acq_rel) != 1) {
            continue;
        }
        const auto status = submit_phase(successor);
        if (status != Status::ok) {
            cancel_graph(status, phase_count_);
            return;
        }
    }
}

void Executor::worker_loop(std::size_t worker_index) noexcept {
    rt::init_fp_env();
    worker_starts_.fetch_add(1, std::memory_order_release);
    while (!stopping_.load(std::memory_order_acquire)) {
        if (!execute_one(worker_index)) {
            std::this_thread::yield();
        }
    }
}

std::size_t Executor::static_worker(
    std::size_t phase_index,
    std::size_t task_index) const noexcept {
    return (phase_index + task_index) % worker_count_;
}

ExecutorStats Executor::stats() const noexcept {
    return ExecutorStats{
        policy_,
        worker_count_,
        queue_capacity_,
        submitted_tasks_.load(std::memory_order_acquire),
        local_executions_.load(std::memory_order_acquire),
        steal_attempts_.load(std::memory_order_acquire),
        successful_steals_.load(std::memory_order_acquire),
        queue_full_rejections_.load(std::memory_order_acquire),
        worker_starts_.load(std::memory_order_acquire),
    };
}

bool Executor::static_assignment(
    std::size_t phase_index,
    std::size_t& worker_index) const noexcept {
    worker_index = kInvalidWorker;
    if (policy_ != ExecutorPolicy::static_deterministic ||
        phase_index >= static_assignments_.size()) {
        return false;
    }
    worker_index = static_assignments_[phase_index];
    return true;
}

} // namespace rt::detail

namespace rt {

Status TaskContext::parallel_for(
    std::size_t item_count,
    std::size_t grain_size,
    RangeTaskCallback callback,
    void* user_data) const noexcept {
    if (!executor_) {
        return Status::invalid_state;
    }
    return executor_->parallel_for(
        *this,
        item_count,
        grain_size,
        callback,
        user_data);
}

Status TaskContext::parallel_reduce(
    std::size_t item_count,
    std::size_t grain_size,
    RangeTaskCallback range_callback,
    ReductionTaskCallback combine_callback,
    void* user_data) const noexcept {
    if (!executor_) {
        return Status::invalid_state;
    }
    return executor_->parallel_reduce(
        *this,
        item_count,
        grain_size,
        range_callback,
        combine_callback,
        user_data);
}

} // namespace rt

#include "executor.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#include <rt/arch.hpp>
#include <rt/numerics.hpp>

namespace {

constexpr std::size_t kInvalidWorker = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kInvalidScratchSlot =
    std::numeric_limits<std::size_t>::max();
constexpr std::size_t kQueueCasAttemptLimit = 64;
constexpr std::size_t kScratchCasAttemptLimit = 64;
constexpr std::size_t kScratchWordBits = 64;
constexpr std::uint8_t kPhaseIdle = 0;
constexpr std::uint8_t kPhaseDispatching = 1;
constexpr std::uint8_t kPhasePending = 2;
constexpr std::uint8_t kPhaseExternalReady = 3;
constexpr std::uint8_t kPhaseFinalized = 4;
constexpr std::uint64_t kHostSlotStateMask = 3;
constexpr std::uint64_t kHostSlotFree = 0;
constexpr std::uint64_t kHostSlotAccepted = 1;
constexpr std::uint64_t kHostSlotRunning = 2;
constexpr std::uint64_t kHostTokenIncrement = 4;
constexpr std::size_t kHostTokenCasAttemptLimit = 64;

} // namespace

namespace rt::detail {

#if defined(_MSC_VER)
#pragma warning(push)
// The producer/consumer cursors intentionally occupy separate cache lines.
#pragma warning(disable : 4324)
#endif

class Executor::Queue final {
    struct Cell {
        std::atomic<std::size_t> sequence{0};
        WorkItem item{};
    };

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

    [[nodiscard]] static constexpr std::size_t cell_size() noexcept {
        return sizeof(Cell);
    }

    void append_control_extents(
        std::vector<LogicalControlExtent>& extents,
        std::uint64_t& next_extent_id) const {
        extents.push_back({
            next_extent_id++,
            ControlExtentOwner::executor,
            this,
            sizeof(*this),
        });
        if (capacity_ != 0) {
            extents.push_back({
                next_extent_id++,
                ControlExtentOwner::executor,
                cells_.get(),
                capacity_ * sizeof(Cell),
            });
        }
    }

private:
    std::unique_ptr<Cell[]> cells_;
    std::size_t capacity_;
    std::size_t mask_;
    alignas(64) std::atomic<std::size_t> enqueue_{0};
    alignas(64) std::atomic<std::size_t> dequeue_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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
    std::size_t task_scratch_bytes,
    std::size_t task_scratch_slots,
    std::size_t scratch_alignment,
    std::span<std::byte> task_scratch_storage,
    OverloadPolicy overload_policy,
    std::span<const GraphDependency> dependencies,
    const HostExecutorAdapter* host_adapter)
    : policy_(policy),
      worker_count_(worker_count),
      queue_capacity_(queue_capacity),
      phase_count_(phase_count),
      task_scratch_bytes_(task_scratch_bytes),
      task_scratch_slots_(task_scratch_slots),
      scratch_alignment_(scratch_alignment),
      overload_policy_(overload_policy),
      task_scratch_storage_(task_scratch_storage),
      initial_indegree_(phase_count, 0),
      current_indegree_(
          phase_count == 0
              ? nullptr
              : std::make_unique<std::atomic<std::uint32_t>[]>(phase_count)),
      phase_states_(
          phase_count == 0
              ? nullptr
              : std::make_unique<std::atomic<std::uint8_t>[]>(phase_count)),
      phase_statuses_(
          phase_count == 0
              ? nullptr
              : std::make_unique<std::atomic<std::int32_t>[]>(phase_count)),
      successor_offsets_(phase_count + 1, 0),
      static_assignments_(phase_count, 0) {
    std::size_t task_scratch_total = 0;
    if (!checked_align_up(
            task_scratch_bytes_,
            scratch_alignment_,
            task_scratch_stride_) ||
        !checked_multiply(
            task_scratch_stride_,
            task_scratch_slots_,
            task_scratch_total)) {
        throw std::bad_alloc();
    }
    if (task_scratch_storage_.size() < task_scratch_total ||
        (task_scratch_total != 0 &&
         (reinterpret_cast<std::uintptr_t>(task_scratch_storage_.data()) &
          (scratch_alignment_ - 1)) != 0)) {
        throw std::invalid_argument("task scratch backing span is invalid");
    }

    free_scratch_word_count_ =
        1 + ((task_scratch_slots_ - 1) / kScratchWordBits);
    free_scratch_words_ =
        std::make_unique<std::atomic<std::uint64_t>[]>(
            free_scratch_word_count_);
    for (std::size_t word = 0;
         word < free_scratch_word_count_;
         ++word) {
        free_scratch_words_[word].store(
            std::numeric_limits<std::uint64_t>::max(),
            std::memory_order_relaxed);
    }
    const auto final_bits = task_scratch_slots_ % kScratchWordBits;
    if (final_bits != 0) {
        free_scratch_words_[free_scratch_word_count_ - 1].store(
            (std::uint64_t{1} << final_bits) - 1,
            std::memory_order_relaxed);
    }

    if (policy_ == ExecutorPolicy::host_adapter) {
        if (host_adapter == nullptr) {
            throw std::invalid_argument("host adapter is required");
        }
        host_adapter_ = *host_adapter;
        host_work_slots_ =
            std::make_unique<HostWorkSlot[]>(task_scratch_slots_);
    } else {
        queues_.reserve(worker_count_);
        threads_ = std::make_unique<NativeThread[]>(worker_count_);
        startup_results_ =
            std::make_unique<ThreadStartupResult[]>(worker_count_);
        worker_entries_ = std::make_unique<WorkerEntry[]>(worker_count_);
        wake_sequences_ =
            std::make_unique<std::atomic<std::uint64_t>[]>(worker_count_);
        for (std::size_t worker = 0; worker < worker_count_; ++worker) {
            queues_.push_back(std::make_unique<Queue>(queue_capacity_));
            worker_entries_[worker] = {this, worker};
            wake_sequences_[worker].store(0, std::memory_order_relaxed);
        }
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
    (void)stop();
}

bool Executor::estimate_control_storage(
    ExecutorPolicy policy,
    std::size_t worker_count,
    std::size_t queue_capacity,
    std::size_t phase_count,
    std::size_t dependency_count,
    std::size_t task_scratch_slots,
    std::size_t& bytes) noexcept {
    bytes = sizeof(Executor);

    const auto add_product =
        [&](std::size_t count, std::size_t element_size) {
            std::size_t product = 0;
            std::size_t total = 0;
            if (!checked_multiply(count, element_size, product) ||
                !checked_add(bytes, product, total)) {
                return false;
            }
            bytes = total;
            return true;
        };

    std::size_t phase_offsets = 0;
    if (!checked_add(phase_count, 1, phase_offsets) ||
        !add_product(phase_count, sizeof(std::uint32_t)) ||
        !add_product(
            phase_count,
            sizeof(std::atomic<std::uint32_t>)) ||
        !add_product(
            phase_count,
            sizeof(std::atomic<std::uint8_t>)) ||
        !add_product(
            phase_count,
            sizeof(std::atomic<std::int32_t>)) ||
        !add_product(phase_offsets, sizeof(std::size_t)) ||
        !add_product(dependency_count, sizeof(std::uint32_t)) ||
        !add_product(phase_count, sizeof(std::size_t))) {
        bytes = 0;
        return false;
    }

    if (policy == ExecutorPolicy::host_adapter) {
        if (!add_product(task_scratch_slots, sizeof(HostWorkSlot))) {
            bytes = 0;
            return false;
        }
    } else {
        std::size_t queue_slots = 0;
        if (!checked_multiply(
                worker_count,
                queue_capacity,
                queue_slots) ||
            !add_product(
                worker_count,
                sizeof(std::unique_ptr<Queue>)) ||
            !add_product(worker_count, sizeof(NativeThread)) ||
            !add_product(worker_count, sizeof(ThreadStartupResult)) ||
            !add_product(worker_count, sizeof(WorkerEntry)) ||
            !add_product(
                worker_count,
                sizeof(std::atomic<std::uint64_t>)) ||
            !add_product(worker_count, sizeof(Queue)) ||
            !add_product(queue_slots, Queue::cell_size())) {
            bytes = 0;
            return false;
        }
    }

    const auto scratch_words =
        task_scratch_slots == 0
        ? std::size_t{0}
        : 1 + ((task_scratch_slots - 1) / kScratchWordBits);
    if (!add_product(
            scratch_words,
            sizeof(std::atomic<std::uint64_t>))) {
        bytes = 0;
        return false;
    }
    return true;
}

Status Executor::start(
    ThreadPolicyProvider& provider,
    ThreadStartupGate& gate,
    const ThreadRolePlan& plan) noexcept {
    if (started_.load(std::memory_order_acquire)) {
        return Status::invalid_state;
    }

    stopping_.store(false, std::memory_order_release);
    submitted_tasks_.store(0, std::memory_order_relaxed);
    local_executions_.store(0, std::memory_order_relaxed);
    steal_attempts_.store(0, std::memory_order_relaxed);
    successful_steals_.store(0, std::memory_order_relaxed);
    queue_full_rejections_.store(0, std::memory_order_relaxed);
    scratch_exhaustions_.store(0, std::memory_order_relaxed);
    worker_starts_.store(0, std::memory_order_relaxed);
    started_.store(true, std::memory_order_release);

    if (policy_ == ExecutorPolicy::host_adapter) {
        for (std::size_t slot = 0;
             slot < task_scratch_slots_;
             ++slot) {
            host_work_slots_[slot].control.store(
                kHostSlotFree,
                std::memory_order_relaxed);
        }
        return Status::ok;
    }
    wait_strategy_ = plan.resolved.wait_strategy;
    for (std::size_t worker = 0; worker < worker_count_; ++worker) {
        wake_sequences_[worker].store(0, std::memory_order_relaxed);
        const auto status = threads_[worker].start(
            provider,
            gate,
            plan,
            worker,
            startup_results_[worker],
            &Executor::worker_entry,
            &worker_entries_[worker]);
        if (status != Status::ok) {
            return status;
        }
    }
    return Status::ok;
}

Status Executor::stop() noexcept {
    (void)started_.exchange(false, std::memory_order_acq_rel);
    stopping_.store(true, std::memory_order_release);
    if (policy_ == ExecutorPolicy::host_adapter) {
        return Status::ok;
    }
    for (std::size_t worker = 0; worker < worker_count_; ++worker) {
        wake_sequences_[worker].fetch_add(1, std::memory_order_release);
        wake_sequences_[worker].notify_all();
    }
    Status first_failure = Status::ok;
    for (std::size_t worker = worker_count_; worker != 0; --worker) {
        const auto status = threads_[worker - 1].cleanup_and_join();
        if (first_failure == Status::ok && status != Status::ok) {
            first_failure = status;
        }
    }
    return first_failure;
}

void Executor::wait_started() const noexcept {
    if (policy_ == ExecutorPolicy::host_adapter) {
        return;
    }
    for (std::size_t worker = 0; worker < worker_count_; ++worker) {
        threads_[worker].wait_started();
    }
}

bool Executor::acquire_scratch_slot(
    std::size_t& scratch_slot) noexcept {
    scratch_slot = kInvalidScratchSlot;
    if (free_scratch_word_count_ == 0) {
        return false;
    }

    const auto first_word =
        scratch_word_hint_.fetch_add(1, std::memory_order_relaxed) %
        free_scratch_word_count_;
    std::size_t cas_attempts = 0;
    for (std::size_t offset = 0;
         offset < free_scratch_word_count_;
         ++offset) {
        const auto word_index =
            (first_word + offset) % free_scratch_word_count_;
        auto available = free_scratch_words_[word_index].load(
            std::memory_order_acquire);
        while (available != 0 &&
               cas_attempts < kScratchCasAttemptLimit) {
            const auto bit =
                static_cast<std::size_t>(std::countr_zero(available));
            const auto mask = std::uint64_t{1} << bit;
            const auto desired = available & ~mask;
            ++cas_attempts;
            if (free_scratch_words_[word_index].compare_exchange_weak(
                    available,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                scratch_slot =
                    (word_index * kScratchWordBits) + bit;
                return scratch_slot < task_scratch_slots_;
            }
            rt::cpu_relax();
        }
        if (cas_attempts >= kScratchCasAttemptLimit) {
            return false;
        }
    }
    return false;
}

void Executor::release_scratch_slot(
    std::size_t scratch_slot) noexcept {
    if (scratch_slot >= task_scratch_slots_) {
        return;
    }
    const auto word = scratch_slot / kScratchWordBits;
    const auto bit = scratch_slot % kScratchWordBits;
    free_scratch_words_[word].fetch_or(
        std::uint64_t{1} << bit,
        std::memory_order_release);
}

Status Executor::reject_overload(
    Status status,
    std::size_t phase_index) noexcept {
    if (overload_policy_ == OverloadPolicy::fail_frame) {
        cancel_graph(status, phase_index);
    }
    return status;
}

Status Executor::submit(
    WorkItem item,
    std::size_t target_worker) noexcept {
    if (!started_.load(std::memory_order_acquire) ||
        target_worker >= worker_count_ ||
        item.group == nullptr) {
        return Status::invalid_state;
    }

    if (!acquire_scratch_slot(item.scratch_slot)) {
        scratch_exhaustions_.fetch_add(1, std::memory_order_relaxed);
        return reject_overload(
            Status::scratch_exhausted,
            item.phase_index);
    }

    item.group->pending.fetch_add(1, std::memory_order_acq_rel);
    if (policy_ == ExecutorPolicy::host_adapter) {
        auto& slot = host_work_slots_[item.scratch_slot];
        auto completion_token = std::uint64_t{0};
        auto sequence = host_completion_sequence_.load(
            std::memory_order_relaxed);
        for (std::size_t attempt = 0;
             sequence != 0 && attempt < kHostTokenCasAttemptLimit;
             ++attempt) {
            const auto next =
                sequence == ~kHostSlotStateMask
                ? std::uint64_t{0}
                : sequence + kHostTokenIncrement;
            if (host_completion_sequence_.compare_exchange_weak(
                    sequence,
                    next,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                completion_token = sequence;
                break;
            }
            rt::cpu_relax();
        }
        if (completion_token == 0) {
            item.group->pending.fetch_sub(1, std::memory_order_acq_rel);
            release_scratch_slot(item.scratch_slot);
            return reject_overload(
                Status::resource_exhausted,
                item.phase_index);
        }
        slot.item = item;
        const auto accepted_control =
            completion_token | kHostSlotAccepted;
        slot.control.store(accepted_control, std::memory_order_release);
        auto* scratch =
            task_scratch_bytes_ == 0
            ? nullptr
            : task_scratch_storage_.data() +
                  (item.scratch_slot * task_scratch_stride_);
        const HostExecutorJob job{
            &Executor::execute_host_job,
            this,
            &slot,
            completion_token,
            scratch,
            task_scratch_bytes_,
        };
        const auto status =
            host_adapter_.submit(host_adapter_.user_data, job);
        if (status == Status::ok) {
            submitted_tasks_.fetch_add(1, std::memory_order_relaxed);
            return Status::ok;
        }

        auto expected = accepted_control;
        if (!slot.control.compare_exchange_strong(
                expected,
                completion_token | kHostSlotFree,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return Status::internal_error;
        }
        item.group->pending.fetch_sub(1, std::memory_order_acq_rel);
        release_scratch_slot(item.scratch_slot);
        if (status == Status::queue_full) {
            queue_full_rejections_.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        return reject_overload(status, item.phase_index);
    }

    if (!queues_[target_worker]->try_push(item)) {
        item.group->pending.fetch_sub(1, std::memory_order_acq_rel);
        release_scratch_slot(item.scratch_slot);
        queue_full_rejections_.fetch_add(1, std::memory_order_relaxed);
        return reject_overload(
            Status::queue_full,
            item.phase_index);
    }
    submitted_tasks_.fetch_add(1, std::memory_order_relaxed);
    wake_sequences_[target_worker].fetch_add(1, std::memory_order_release);
    wake_sequences_[target_worker].notify_one();
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
        phase_states_[phase].store(
            kPhaseIdle,
            std::memory_order_relaxed);
        phase_statuses_[phase].store(
            static_cast<std::int32_t>(Status::ok),
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
        if (policy_ == ExecutorPolicy::host_adapter &&
            host_adapter_.try_execute_one(host_adapter_.user_data)) {
            continue;
        }
        if (helping_worker < worker_count_ &&
            execute_one(helping_worker)) {
            continue;
        }
        std::this_thread::yield();
    }
    return group.result();
}

bool Executor::execute_one(std::size_t worker_index) noexcept {
    if (policy_ == ExecutorPolicy::host_adapter) {
        return false;
    }
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

void Executor::execute_host_job(
    void* execution_context,
    void* completion_context,
    std::uint64_t completion_token,
    std::uint32_t worker_index) noexcept {
    if (!execution_context || !completion_context) {
        return;
    }
    auto& executor = *static_cast<Executor*>(execution_context);
    auto& slot = *static_cast<HostWorkSlot*>(completion_context);
    executor.execute_host_slot(
        slot,
        completion_token,
        static_cast<std::size_t>(worker_index));
}

void Executor::execute_host_slot(
    HostWorkSlot& slot,
    std::uint64_t completion_token,
    std::size_t worker_index) noexcept {
    const auto token =
        completion_token & ~kHostSlotStateMask;
    auto expected = token | kHostSlotAccepted;
    if (token == 0 || completion_token != token ||
        !slot.control.compare_exchange_strong(
            expected,
            token | kHostSlotRunning,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return;
    }

    const auto item = slot.item;
    slot.control.store(
        token | kHostSlotFree,
        std::memory_order_release);
    if (worker_index >= worker_count_) {
        item.group->record(Status::internal_error);
        if (item.kind == WorkKind::phase) {
            cancel_graph(Status::internal_error, item.phase_index);
        }
        release_scratch_slot(item.scratch_slot);
        item.group->pending.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    local_executions_.fetch_add(1, std::memory_order_relaxed);
    execute(worker_index, item);
}

void Executor::execute(
    std::size_t worker_index,
    const WorkItem& item) noexcept {
    std::span<std::byte> scratch;
    if (task_scratch_bytes_ != 0 &&
        item.scratch_slot < task_scratch_slots_) {
        scratch = std::span<std::byte>(
            task_scratch_storage_.data() +
                (item.scratch_slot * task_scratch_stride_),
            task_scratch_bytes_);
    }
    TaskContext context(
        *this,
        worker_index,
        item.phase_index,
        item.task_index,
        scratch);
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
    release_scratch_slot(item.scratch_slot);
    item.group->pending.fetch_sub(1, std::memory_order_acq_rel);
}

void Executor::execute_phase(
    std::size_t,
    const WorkItem& item,
    const TaskContext& context) noexcept {
    if (graph_cancelled_.load(std::memory_order_acquire)) {
        return;
    }

    graph_group_.pending.fetch_add(1, std::memory_order_acq_rel);
    auto expected_state = kPhaseIdle;
    if (!phase_states_[item.phase_index].compare_exchange_strong(
            expected_state,
            kPhaseDispatching,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        graph_group_.pending.fetch_sub(1, std::memory_order_acq_rel);
        cancel_graph(Status::internal_error, item.phase_index);
        return;
    }

    PhaseTaskDispatch dispatch{
        Status::callback_failed,
        false,
    };
    try {
        dispatch = phase_callback_(
            phase_user_data_,
            item.phase_index,
            context);
    } catch (...) {
        dispatch = {
            Status::callback_failed,
            false,
        };
    }
    graph_callbacks_executed_.fetch_add(1, std::memory_order_relaxed);

    if (!dispatch.pending) {
        expected_state = kPhaseDispatching;
        if (!phase_states_[item.phase_index].compare_exchange_strong(
                expected_state,
                kPhaseFinalized,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            cancel_graph(Status::internal_error, item.phase_index);
            graph_group_.pending.fetch_sub(
                1,
                std::memory_order_acq_rel);
            return;
        }
        finish_phase(item.phase_index, dispatch.status);
        return;
    }

    if (dispatch.status != Status::ok) {
        expected_state = kPhaseDispatching;
        if (phase_states_[item.phase_index].compare_exchange_strong(
                expected_state,
                kPhaseFinalized,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            finish_phase(item.phase_index, dispatch.status);
        } else {
            cancel_graph(Status::internal_error, item.phase_index);
            graph_group_.pending.fetch_sub(
                1,
                std::memory_order_acq_rel);
        }
        return;
    }

    expected_state = kPhaseDispatching;
    if (phase_states_[item.phase_index].compare_exchange_strong(
            expected_state,
            kPhasePending,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return;
    }
    if (expected_state != kPhaseExternalReady) {
        cancel_graph(Status::internal_error, item.phase_index);
        graph_group_.pending.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    expected_state = kPhaseExternalReady;
    if (!phase_states_[item.phase_index].compare_exchange_strong(
            expected_state,
            kPhaseFinalized,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        cancel_graph(Status::internal_error, item.phase_index);
        graph_group_.pending.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    finish_phase(
        item.phase_index,
        static_cast<Status>(
            phase_statuses_[item.phase_index].load(
                std::memory_order_acquire)));
}

void Executor::finish_phase(
    std::size_t phase_index,
    Status status) noexcept {
    if (status != Status::ok) {
        cancel_graph(status, phase_index);
        graph_group_.pending.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    if (graph_cancelled_.load(std::memory_order_acquire)) {
        graph_group_.pending.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    const auto begin = successor_offsets_[phase_index];
    const auto end = successor_offsets_[phase_index + 1];
    for (std::size_t cursor = begin; cursor < end; ++cursor) {
        const auto successor = successors_[cursor];
        if (current_indegree_[successor].fetch_sub(
                1,
                std::memory_order_acq_rel) != 1) {
            continue;
        }
        const auto submit_status = submit_phase(successor);
        if (submit_status != Status::ok) {
            cancel_graph(submit_status, phase_count_);
            break;
        }
    }
    graph_group_.pending.fetch_sub(1, std::memory_order_acq_rel);
}

Status Executor::complete_external(
    std::size_t phase_index,
    Status status) noexcept {
    if (!started_.load(std::memory_order_acquire) ||
        phase_index >= phase_count_) {
        return Status::invalid_argument;
    }
    phase_statuses_[phase_index].store(
        static_cast<std::int32_t>(status),
        std::memory_order_release);
    auto expected = kPhasePending;
    if (phase_states_[phase_index].compare_exchange_strong(
            expected,
            kPhaseFinalized,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        finish_phase(phase_index, status);
        return Status::ok;
    }
    if (expected != kPhaseDispatching) {
        return Status::invalid_state;
    }
    expected = kPhaseDispatching;
    if (phase_states_[phase_index].compare_exchange_strong(
            expected,
            kPhaseExternalReady,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return Status::ok;
    }
    if (expected == kPhasePending) {
        expected = kPhasePending;
        if (phase_states_[phase_index].compare_exchange_strong(
                expected,
                kPhaseFinalized,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            finish_phase(phase_index, status);
            return Status::ok;
        }
    }
    return Status::invalid_state;
}

void Executor::worker_loop(std::size_t worker_index) noexcept {
    rt::init_fp_env();
    worker_starts_.fetch_add(1, std::memory_order_release);
    while (!stopping_.load(std::memory_order_acquire)) {
        if (execute_one(worker_index)) {
            continue;
        }
        if (wait_strategy_ == WaitStrategy::spin) {
            rt::cpu_relax();
        } else if (wait_strategy_ == WaitStrategy::yield) {
            std::this_thread::yield();
        } else {
            const auto observed = wake_sequences_[worker_index].load(
                std::memory_order_acquire);
            if (!stopping_.load(std::memory_order_acquire) &&
                !execute_one(worker_index) &&
                wake_sequences_[worker_index].load(
                    std::memory_order_acquire) == observed) {
                wake_sequences_[worker_index].wait(
                    observed,
                    std::memory_order_relaxed);
            }
        }
    }
}

void Executor::worker_entry(void* entry) noexcept {
    auto& worker = *static_cast<WorkerEntry*>(entry);
    worker.executor->worker_loop(worker.worker_index);
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
        scratch_exhaustions_.load(std::memory_order_acquire),
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

void Executor::append_control_extents(
    std::vector<LogicalControlExtent>& extents,
    std::uint64_t& next_extent_id) const {
    const auto add = [&](const void* data, std::size_t count, std::size_t size) {
        if (count == 0) {
            return;
        }
        extents.push_back({
            next_extent_id++,
            ControlExtentOwner::executor,
            data,
            count * size,
        });
    };
    add(this, 1, sizeof(*this));
    add(queues_.data(), queues_.capacity(), sizeof(queues_[0]));
    for (const auto& queue : queues_) {
        queue->append_control_extents(extents, next_extent_id);
    }
    add(threads_.get(), policy_ == ExecutorPolicy::host_adapter ? 0 : worker_count_, sizeof(NativeThread));
    add(startup_results_.get(), policy_ == ExecutorPolicy::host_adapter ? 0 : worker_count_, sizeof(ThreadStartupResult));
    add(worker_entries_.get(), policy_ == ExecutorPolicy::host_adapter ? 0 : worker_count_, sizeof(WorkerEntry));
    add(wake_sequences_.get(), policy_ == ExecutorPolicy::host_adapter ? 0 : worker_count_, sizeof(std::atomic<std::uint64_t>));
    add(host_work_slots_.get(), policy_ == ExecutorPolicy::host_adapter ? task_scratch_slots_ : 0, sizeof(HostWorkSlot));
    add(free_scratch_words_.get(), free_scratch_word_count_, sizeof(std::atomic<std::uint64_t>));
    add(initial_indegree_.data(), initial_indegree_.capacity(), sizeof(initial_indegree_[0]));
    add(current_indegree_.get(), phase_count_, sizeof(std::atomic<std::uint32_t>));
    add(phase_states_.get(), phase_count_, sizeof(std::atomic<std::uint8_t>));
    add(phase_statuses_.get(), phase_count_, sizeof(std::atomic<std::int32_t>));
    add(successor_offsets_.data(), successor_offsets_.capacity(), sizeof(successor_offsets_[0]));
    add(successors_.data(), successors_.capacity(), sizeof(successors_[0]));
    add(static_assignments_.data(), static_assignments_.capacity(), sizeof(static_assignments_[0]));
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

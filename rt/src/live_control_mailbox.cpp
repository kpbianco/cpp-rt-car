#include "live_control_mailbox.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint32_t kInvalidIndex =
    std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t kInvalidSequence =
    std::numeric_limits<std::uint64_t>::max();

template <std::size_t Size>
[[nodiscard]] bool all_zero(
    const std::array<std::byte, Size>& bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte value) {
        return value == std::byte{0};
    });
}

[[nodiscard]] bool checked_add(
    std::size_t left,
    std::size_t right,
    std::size_t& output) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    output = left + right;
    return true;
}

[[nodiscard]] bool checked_multiply(
    std::size_t left,
    std::size_t right,
    std::size_t& output) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    output = left * right;
    return true;
}

void increment(std::atomic<std::uint64_t>& value) noexcept {
    (void)value.fetch_add(1, std::memory_order_relaxed);
}

class FlagGuard final {
public:
    explicit FlagGuard(std::atomic_flag& flag) noexcept : flag_(flag) {}
    ~FlagGuard() { flag_.clear(std::memory_order_release); }
    FlagGuard(const FlagGuard&) = delete;
    FlagGuard& operator=(const FlagGuard&) = delete;

private:
    std::atomic_flag& flag_;
};

} // namespace

namespace rt {

std::uint64_t live_control_payload_digest(
    std::span<const std::byte> payload) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const auto value : payload) {
        hash ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(value));
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace rt

namespace rt::detail {

static_assert(
    std::atomic<std::uint64_t>::is_always_lock_free,
    "live-control mailboxes require lock-free 64-bit atomics");

struct LiveControlMailboxSet::Impl {
    enum class SlotState : std::uint8_t {
        free = 0,
        writing = 1,
        staged = 2,
        boundary_owned = 3,
        committed = 4,
        replaced = 5,
        missed = 6,
        stopped = 7,
    };

    struct RateTarget {
        std::uint64_t release_time_ns = 0;
        std::uint64_t release_sequence = 0;
        std::uint32_t domain_registration_index = 0;
        std::uint32_t phase_index = 0;
        std::uint32_t substep_ordinal = 0;
        std::atomic<bool> closed{false};
    };

    struct Slot {
        std::atomic<SlotState> state{SlotState::free};
        std::atomic<std::uint64_t> terminal_generation{0};
        LiveControlUpdateRecord record{};
    };

    struct Mailbox {
        LiveControlMailboxRegistration registration{};
        std::uint32_t producer_count = 0;
        std::atomic_flag reservation = ATOMIC_FLAG_INIT;
        std::atomic<bool> reclaiming{false};
        std::atomic<std::uint32_t> inspection_readers{0};
        std::unique_ptr<Slot[]> slots{};
        std::unique_ptr<std::byte[]> payload_storage{};
        std::atomic<std::uint64_t> next_sequence{1};
        std::atomic<std::uint32_t> occupancy{0};
        std::atomic<std::uint64_t> accepted{0};
        std::atomic<std::uint64_t> invalid{0};
        std::atomic<std::uint64_t> full{0};
        std::atomic<std::uint64_t> busy{0};
        std::atomic<std::uint64_t> stale{0};
        std::atomic<std::uint64_t> stopped{0};
        std::atomic<std::uint64_t> exhausted{0};
        std::atomic<std::uint64_t> missed{0};
    };

    struct Candidate {
        std::uint32_t mailbox_index = 0;
        std::uint32_t slot_index = 0;
        std::uint64_t mailbox_identity = 0;
        std::uint64_t mailbox_sequence = 0;
        bool survivor = true;
        std::array<std::byte, 7> reserved{};
    };

    struct Generation {
        std::unique_ptr<LiveControlRecordView[]> records{};
        std::unique_ptr<std::byte[]> payloads{};
        LiveControlGenerationView view{};
    };

    struct Producer {
        std::uint64_t mailbox_identity = 0;
        std::uint64_t producer_identity = 0;
        std::uint32_t mailbox_index = kInvalidIndex;
        std::uint64_t first_sequence = 1;
        std::atomic<std::uint64_t> next_sequence{1};
    };

    LiveControlPolicy policy{};
    std::uint64_t runtime_id = 0;
    std::uint64_t configuration_generation = 0;
    std::atomic<bool> admission_open{true};
    std::vector<std::unique_ptr<Mailbox>> mailboxes{};
    std::unique_ptr<Producer[]> producers{};
    std::size_t producer_count = 0;
    std::unique_ptr<RateTarget[]> rate_targets{};
    std::size_t rate_target_count = 0;
    std::unique_ptr<Candidate[]> candidates{};
    std::array<Generation, 2> generations{};
    std::atomic<std::uint8_t> active_generation{0};
    std::atomic<bool> has_generation{false};
    std::atomic<bool> host_boundary_started{false};
    std::atomic<std::uint64_t> last_host_boundary{0};
    std::atomic<std::uint64_t> committed{0};
    std::atomic<std::uint64_t> replaced{0};
    std::atomic<std::uint64_t> missed{0};
    std::atomic<std::uint64_t> stopped{0};
    std::atomic<std::uint64_t> inspection_version{0};
    std::atomic<std::uint64_t> latest_generation_identity{0};
    std::atomic<std::uint64_t> latest_frame_index{kInvalidSequence};
    std::atomic<std::uint64_t> latest_rate_sequence{kInvalidSequence};
    std::atomic<std::uint32_t> latest_reference_index{kInvalidIndex};
    std::atomic<std::uint32_t> latest_domain_index{kInvalidIndex};
    std::atomic<std::uint32_t> latest_phase_index{kInvalidIndex};
    std::atomic<std::uint32_t> latest_substep{kInvalidIndex};
    std::atomic<std::uint32_t> latest_survivor_count{0};
    std::atomic<LiveControlTargetKind> latest_target_kind{
        LiveControlTargetKind::host_frame};
    std::size_t total_record_capacity = 0;
    std::size_t total_payload_storage_bytes = 0;
};

namespace {

[[nodiscard]] std::size_t find_mailbox(
    const LiveControlMailboxSet::Impl& impl,
    std::uint64_t identity) noexcept {
    for (std::size_t index = 0; index < impl.mailboxes.size(); ++index) {
        if (impl.mailboxes[index]->registration.mailbox_identity == identity) {
            return index;
        }
    }
    return impl.mailboxes.size();
}

[[nodiscard]] bool update_kind_valid(LiveControlUpdateKind kind) noexcept {
    return kind >= LiveControlUpdateKind::scenario_parameters &&
        kind <= LiveControlUpdateKind::clear_fault;
}

[[nodiscard]] bool target_valid(
    const LiveControlMailboxSet::Impl& impl,
    const LiveControlUpdateRecord& update) noexcept {
    if (update.target_kind == LiveControlTargetKind::host_frame) {
        return update.target_frame_index != kInvalidSequence &&
            update.reference_release_index == kInvalidIndex &&
            update.rate_domain_registration_index == kInvalidIndex &&
            update.phase_index == kInvalidIndex &&
            update.rate_substep_ordinal == kInvalidIndex &&
            update.rate_release_sequence == kInvalidSequence;
    }
    if (update.target_kind != LiveControlTargetKind::rate_release ||
        update.target_frame_index != kInvalidSequence ||
        update.reference_release_index >= impl.rate_target_count) {
        return false;
    }
    const auto& target = impl.rate_targets[update.reference_release_index];
    return update.rate_domain_registration_index ==
               target.domain_registration_index &&
        update.phase_index == target.phase_index &&
        update.rate_substep_ordinal == target.substep_ordinal &&
        update.rate_release_sequence == target.release_sequence;
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        hash ^= static_cast<std::uint8_t>(value & 0xffu);
        hash *= kFnvPrime;
        value >>= 8u;
    }
}

void hash_target(
    std::uint64_t& hash,
    const LiveControlBoundaryTarget& target) noexcept {
    hash_u64(hash, static_cast<std::uint8_t>(target.kind));
    hash_u64(hash, target.frame_index);
    hash_u64(hash, target.rate_release_sequence);
    hash_u64(hash, target.reference_release_index);
    hash_u64(hash, target.rate_domain_registration_index);
    hash_u64(hash, target.phase_index);
    hash_u64(hash, target.rate_substep_ordinal);
}

[[nodiscard]] LiveControlRecordStatus public_status(
    LiveControlMailboxSet::Impl::SlotState state) noexcept {
    using State = LiveControlMailboxSet::Impl::SlotState;
    switch (state) {
    case State::staged:
    case State::boundary_owned:
        return LiveControlRecordStatus::staged;
    case State::committed:
        return LiveControlRecordStatus::committed;
    case State::replaced:
        return LiveControlRecordStatus::replaced;
    case State::missed:
        return LiveControlRecordStatus::missed;
    case State::stopped:
        return LiveControlRecordStatus::stopped;
    case State::free:
    case State::writing:
        return LiveControlRecordStatus::staged;
    }
    return LiveControlRecordStatus::staged;
}

[[nodiscard]] bool structurally_valid(
    const LiveControlMailboxSet::Impl& impl,
    const LiveControlMailboxSet::Impl::Mailbox& mailbox,
    const LiveControlMailboxSet::Impl::Producer& producer,
    const LiveControlUpdateRecord& update,
    std::span<const std::byte> payload) noexcept {
    const auto alignment = update.payload_alignment;
    const auto alignment_valid = alignment != 0 &&
        alignment <= live_control_payload_alignment_limit &&
        (alignment & (alignment - 1u)) == 0 &&
        (payload.size() % alignment) == 0;
    const auto zero_payload_valid =
        (update.update_kind == LiveControlUpdateKind::clear_fault) ==
        payload.empty();
    return update.schema_version == live_control_schema_version &&
        update.record_size == sizeof(LiveControlUpdateRecord) &&
        update.mailbox_sequence == 0 &&
        update.runtime_id == impl.runtime_id &&
        update.configuration_generation == impl.configuration_generation &&
        update.mailbox_identity == producer.mailbox_identity &&
        update.mailbox_identity == mailbox.registration.mailbox_identity &&
        update.producer_identity == producer.producer_identity &&
        update.producer_sequence != 0 &&
        update.payload_bytes == payload.size() &&
        payload.size() <= mailbox.registration.payload_bytes_per_record &&
        update.payload_digest == live_control_payload_digest(payload) &&
        update.policy_flags ==
            live_control_payload_canonical_little_endian &&
        update_kind_valid(update.update_kind) && alignment_valid &&
        zero_payload_valid && target_valid(impl, update) &&
        all_zero(update.reserved);
}

void count_result(
    LiveControlMailboxSet::Impl::Mailbox& mailbox,
    LiveControlAdmissionResult result) noexcept {
    switch (result) {
    case LiveControlAdmissionResult::accepted:
        increment(mailbox.accepted);
        return;
    case LiveControlAdmissionResult::invalid:
        increment(mailbox.invalid);
        return;
    case LiveControlAdmissionResult::full:
        increment(mailbox.full);
        return;
    case LiveControlAdmissionResult::busy:
        increment(mailbox.busy);
        return;
    case LiveControlAdmissionResult::stale:
        increment(mailbox.stale);
        return;
    case LiveControlAdmissionResult::stopped:
        increment(mailbox.stopped);
        return;
    case LiveControlAdmissionResult::exhausted:
        increment(mailbox.exhausted);
        return;
    case LiveControlAdmissionResult::missed:
        increment(mailbox.missed);
        return;
    }
}

[[nodiscard]] bool target_closed(
    const LiveControlMailboxSet::Impl& impl,
    const LiveControlUpdateRecord& update) noexcept {
    if (update.target_kind == LiveControlTargetKind::host_frame) {
        return impl.host_boundary_started.load(std::memory_order_acquire) &&
            update.target_frame_index <=
                impl.last_host_boundary.load(std::memory_order_acquire);
    }
    return update.reference_release_index >= impl.rate_target_count ||
        impl.rate_targets[update.reference_release_index].closed.load(
            std::memory_order_acquire);
}

[[nodiscard]] bool terminal_state(
    LiveControlMailboxSet::Impl::SlotState state) noexcept {
    using State = LiveControlMailboxSet::Impl::SlotState;
    return state == State::committed || state == State::replaced ||
        state == State::missed || state == State::stopped;
}

} // namespace

LiveControlMailboxSet::LiveControlMailboxSet(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

LiveControlMailboxSet::~LiveControlMailboxSet() = default;

Status LiveControlMailboxSet::create(
    const LiveControlPolicy& policy,
    std::uint64_t runtime_id,
    std::uint64_t configuration_generation,
    std::size_t memory_budget_bytes,
    std::span<const LiveControlMailboxRegistration> mailbox_declarations,
    std::span<const LiveControlProducerRegistration> producer_declarations,
    std::span<const ReferenceRelease> rate_releases,
    std::unique_ptr<LiveControlMailboxSet>& output,
    const char*& diagnostic) noexcept {
    output.reset();
    diagnostic = nullptr;
    if (policy.schema_version != live_control_schema_version ||
        policy.struct_size != sizeof(LiveControlPolicy) ||
        policy.policy_identity == 0 || runtime_id == 0 ||
        configuration_generation == 0 ||
        policy.mailbox_capacity == 0 ||
        policy.mailbox_capacity > live_control_mailbox_capacity_limit ||
        policy.producer_capacity == 0 ||
        policy.producer_capacity > live_control_producer_capacity_limit ||
        policy.record_capacity == 0 ||
        policy.record_capacity > live_control_record_capacity_limit ||
        policy.payload_bytes_per_record == 0 ||
        policy.payload_bytes_per_record > live_control_payload_bytes_limit ||
        policy.total_payload_storage_bytes == 0 ||
        policy.total_payload_storage_bytes > live_control_total_storage_limit ||
        policy.total_payload_storage_bytes >
            std::numeric_limits<std::size_t>::max() ||
        policy.admission_policy != LiveControlAdmissionPolicy::reject_new ||
        policy.reset_rule != LiveControlResetRule::discard_with_runtime ||
        !all_zero(policy.reserved) || mailbox_declarations.empty() ||
        mailbox_declarations.size() > policy.mailbox_capacity ||
        producer_declarations.empty() ||
        producer_declarations.size() > policy.producer_capacity) {
        diagnostic = "live-control policy or declaration counts are invalid";
        return Status::invalid_config;
    }

    std::size_t total_records = 0;
    std::size_t total_payload = 0;
    for (std::size_t index = 0; index < mailbox_declarations.size(); ++index) {
        const auto& declaration = mailbox_declarations[index];
        if (declaration.schema_version != live_control_schema_version ||
            declaration.struct_size != sizeof(LiveControlMailboxRegistration) ||
            declaration.mailbox_identity == 0 ||
            declaration.record_capacity == 0 ||
            declaration.payload_bytes_per_record == 0 ||
            declaration.payload_bytes_per_record >
                policy.payload_bytes_per_record ||
            !all_zero(declaration.reserved)) {
            diagnostic = "live-control mailbox declaration is invalid";
            return Status::invalid_config;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (mailbox_declarations[prior].mailbox_identity ==
                declaration.mailbox_identity) {
                diagnostic = "live-control mailbox identity is duplicated";
                return Status::invalid_config;
            }
        }
        std::size_t storage = 0;
        if (!checked_add(total_records, declaration.record_capacity,
                         total_records) ||
            !checked_multiply(declaration.record_capacity,
                              declaration.payload_bytes_per_record, storage) ||
            !checked_add(total_payload, storage, total_payload) ||
            total_records > policy.record_capacity ||
            total_payload > policy.total_payload_storage_bytes) {
            diagnostic = "live-control mailbox storage exceeds policy bounds";
            return Status::capacity_exceeded;
        }
    }

    for (std::size_t index = 0; index < producer_declarations.size(); ++index) {
        const auto& declaration = producer_declarations[index];
        if (declaration.schema_version != live_control_schema_version ||
            declaration.struct_size != sizeof(LiveControlProducerRegistration) ||
            declaration.mailbox_identity == 0 ||
            declaration.producer_identity == 0 ||
            declaration.first_sequence == 0 ||
            declaration.first_sequence == kInvalidSequence ||
            declaration.reserved != 0) {
            diagnostic = "live-control producer declaration is invalid";
            return Status::invalid_config;
        }
        bool mailbox_found = false;
        for (const auto& mailbox : mailbox_declarations) {
            mailbox_found = mailbox_found ||
                mailbox.mailbox_identity == declaration.mailbox_identity;
        }
        if (!mailbox_found) {
            diagnostic = "live-control producer names an unknown mailbox";
            return Status::invalid_config;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (producer_declarations[prior].producer_identity ==
                declaration.producer_identity) {
                diagnostic = "live-control producer identity is duplicated";
                return Status::invalid_config;
            }
        }
    }

    std::size_t minimum_control_bytes =
        sizeof(LiveControlMailboxSet) + sizeof(Impl);
    std::size_t bytes = 0;
    if (!checked_multiply(
            mailbox_declarations.size(),
            sizeof(std::unique_ptr<Impl::Mailbox>),
            bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes)) {
        diagnostic = "live-control control storage overflows";
        return Status::capacity_exceeded;
    }
    for (const auto& declaration : mailbox_declarations) {
        if (!checked_add(
                minimum_control_bytes,
                sizeof(Impl::Mailbox),
                minimum_control_bytes) ||
            !checked_multiply(
                declaration.record_capacity,
                sizeof(Impl::Slot),
                bytes) ||
            !checked_add(minimum_control_bytes, bytes, minimum_control_bytes) ||
            !checked_multiply(
                declaration.record_capacity,
                declaration.payload_bytes_per_record,
                bytes) ||
            !checked_add(minimum_control_bytes, bytes, minimum_control_bytes)) {
            diagnostic = "live-control control storage overflows";
            return Status::capacity_exceeded;
        }
    }
    if (!checked_multiply(
            producer_declarations.size(), sizeof(Impl::Producer), bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes) ||
        !checked_multiply(
            rate_releases.size(), sizeof(Impl::RateTarget), bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes) ||
        !checked_multiply(total_records, sizeof(Impl::Candidate), bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes) ||
        !checked_multiply(total_records, sizeof(LiveControlRecordView), bytes) ||
        !checked_multiply(bytes, 2u, bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes) ||
        !checked_multiply(total_payload, 2u, bytes) ||
        !checked_add(minimum_control_bytes, bytes, minimum_control_bytes)) {
        diagnostic = "live-control control storage overflows";
        return Status::capacity_exceeded;
    }
    if (minimum_control_bytes > memory_budget_bytes) {
        diagnostic = "live-control control storage exceeds Runtime memory budget";
        return Status::invalid_config;
    }

    try {
        auto impl = std::make_unique<Impl>();
        impl->policy = policy;
        impl->runtime_id = runtime_id;
        impl->configuration_generation = configuration_generation;
        impl->total_record_capacity = total_records;
        impl->total_payload_storage_bytes = total_payload;
        impl->mailboxes.reserve(mailbox_declarations.size());
        for (const auto& declaration : mailbox_declarations) {
            auto mailbox = std::make_unique<Impl::Mailbox>();
            mailbox->registration = declaration;
            mailbox->slots =
                std::make_unique<Impl::Slot[]>(declaration.record_capacity);
            std::size_t storage = 0;
            (void)checked_multiply(declaration.record_capacity,
                                   declaration.payload_bytes_per_record,
                                   storage);
            mailbox->payload_storage = std::make_unique<std::byte[]>(storage);
            std::fill_n(mailbox->payload_storage.get(), storage, std::byte{0});
            impl->mailboxes.push_back(std::move(mailbox));
        }
        impl->producer_count = producer_declarations.size();
        impl->producers =
            std::make_unique<Impl::Producer[]>(impl->producer_count);
        for (std::size_t index = 0; index < impl->producer_count; ++index) {
            const auto& declaration = producer_declarations[index];
            auto& producer = impl->producers[index];
            producer.mailbox_identity = declaration.mailbox_identity;
            producer.producer_identity = declaration.producer_identity;
            producer.mailbox_index = static_cast<std::uint32_t>(
                find_mailbox(*impl, declaration.mailbox_identity));
            producer.first_sequence = declaration.first_sequence;
            producer.next_sequence.store(
                declaration.first_sequence, std::memory_order_relaxed);
            ++impl->mailboxes[producer.mailbox_index]->producer_count;
        }
        impl->rate_target_count = rate_releases.size();
        impl->rate_targets =
            std::make_unique<Impl::RateTarget[]>(impl->rate_target_count);
        for (std::size_t index = 0; index < rate_releases.size(); ++index) {
            const auto& release = rate_releases[index];
            if (release.domain_registration_index > kInvalidIndex) {
                diagnostic = "live-control rate target identity overflows";
                return Status::capacity_exceeded;
            }
            auto& target = impl->rate_targets[index];
            target.release_time_ns = release.release_time_ns;
            target.release_sequence = release.domain_release_sequence;
            target.domain_registration_index =
                static_cast<std::uint32_t>(release.domain_registration_index);
            target.phase_index = release.phase.index();
            target.substep_ordinal = release.substep_ordinal;
        }
        impl->candidates = std::make_unique<Impl::Candidate[]>(total_records);
        for (auto& generation : impl->generations) {
            generation.records =
                std::make_unique<LiveControlRecordView[]>(total_records);
            generation.payloads = std::make_unique<std::byte[]>(total_payload);
            std::fill_n(
                generation.payloads.get(), total_payload, std::byte{0});
        }
        output = std::unique_ptr<LiveControlMailboxSet>(
            new LiveControlMailboxSet(std::move(impl)));
    } catch (const std::bad_alloc&) {
        diagnostic = "live-control mailbox allocation failed";
        return Status::resource_exhausted;
    } catch (...) {
        diagnostic = "live-control mailbox construction failed";
        return Status::internal_error;
    }
    return Status::ok;
}

Status LiveControlMailboxSet::producer_handle(
    std::uint64_t mailbox_identity,
    std::uint64_t producer_identity,
    LiveControlProducerHandle& handle) const noexcept {
    if (!impl_) {
        return Status::invalid_state;
    }
    for (std::size_t index = 0; index < impl_->producer_count; ++index) {
        const auto& producer = impl_->producers[index];
        if (producer.mailbox_identity == mailbox_identity &&
            producer.producer_identity == producer_identity) {
            handle = {
                impl_->runtime_id,
                impl_->configuration_generation,
                mailbox_identity,
                producer_identity,
                static_cast<std::uint32_t>(index),
                0,
            };
            return Status::ok;
        }
    }
    return Status::invalid_handle;
}

LiveControlAdmissionResult LiveControlMailboxSet::stage(
    LiveControlProducerHandle handle,
    const LiveControlUpdateRecord& update,
    std::span<const std::byte> payload) noexcept {
    if (!impl_ || !handle.valid() ||
        handle.runtime_id != impl_->runtime_id ||
        handle.configuration_generation != impl_->configuration_generation ||
        handle.producer_index >= impl_->producer_count) {
        return LiveControlAdmissionResult::stale;
    }
    auto& producer = impl_->producers[handle.producer_index];
    if (producer.mailbox_identity != handle.mailbox_identity ||
        producer.producer_identity != handle.producer_identity ||
        producer.mailbox_index >= impl_->mailboxes.size()) {
        return LiveControlAdmissionResult::stale;
    }
    auto& mailbox = *impl_->mailboxes[producer.mailbox_index];
    if (!impl_->admission_open.load(std::memory_order_acquire)) {
        count_result(mailbox, LiveControlAdmissionResult::stopped);
        return LiveControlAdmissionResult::stopped;
    }
    const auto observed_producer_sequence =
        producer.next_sequence.load(std::memory_order_acquire);
    if (observed_producer_sequence == kInvalidSequence) {
        count_result(mailbox, LiveControlAdmissionResult::exhausted);
        return LiveControlAdmissionResult::exhausted;
    }
    if (update.producer_sequence != observed_producer_sequence ||
        update.runtime_id != handle.runtime_id ||
        update.configuration_generation != handle.configuration_generation ||
        update.mailbox_identity != handle.mailbox_identity ||
        update.producer_identity != handle.producer_identity) {
        count_result(mailbox, LiveControlAdmissionResult::stale);
        return LiveControlAdmissionResult::stale;
    }
    if (!structurally_valid(*impl_, mailbox, producer, update, payload)) {
        count_result(mailbox, LiveControlAdmissionResult::invalid);
        return LiveControlAdmissionResult::invalid;
    }
    if (mailbox.reservation.test_and_set(std::memory_order_acquire)) {
        count_result(mailbox, LiveControlAdmissionResult::busy);
        return LiveControlAdmissionResult::busy;
    }
    FlagGuard guard(mailbox.reservation);
    if (!impl_->admission_open.load(std::memory_order_acquire)) {
        count_result(mailbox, LiveControlAdmissionResult::stopped);
        return LiveControlAdmissionResult::stopped;
    }
    const auto expected_producer_sequence =
        producer.next_sequence.load(std::memory_order_relaxed);
    if (expected_producer_sequence == kInvalidSequence) {
        count_result(mailbox, LiveControlAdmissionResult::exhausted);
        return LiveControlAdmissionResult::exhausted;
    }
    if (update.producer_sequence != expected_producer_sequence ||
        update.runtime_id != handle.runtime_id ||
        update.configuration_generation != handle.configuration_generation ||
        update.mailbox_identity != handle.mailbox_identity ||
        update.producer_identity != handle.producer_identity) {
        count_result(mailbox, LiveControlAdmissionResult::stale);
        return LiveControlAdmissionResult::stale;
    }
    const auto mailbox_sequence =
        mailbox.next_sequence.load(std::memory_order_relaxed);
    if (mailbox_sequence == kInvalidSequence) {
        count_result(mailbox, LiveControlAdmissionResult::exhausted);
        return LiveControlAdmissionResult::exhausted;
    }

    using SlotState = Impl::SlotState;
    std::size_t slot_index = mailbox.registration.record_capacity;
    for (std::size_t index = 0;
         index < mailbox.registration.record_capacity; ++index) {
        auto expected_state = SlotState::free;
        if (mailbox.slots[index].state.compare_exchange_strong(
                expected_state,
                SlotState::writing,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            slot_index = index;
            break;
        }
    }
    if (slot_index == mailbox.registration.record_capacity) {
        bool expected_reclaiming = false;
        if (mailbox.reclaiming.compare_exchange_strong(
                expected_reclaiming,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            if (mailbox.inspection_readers.load(std::memory_order_acquire) ==
                0) {
                for (std::size_t index = 0;
                     index < mailbox.registration.record_capacity; ++index) {
                    auto state = mailbox.slots[index].state.load(
                        std::memory_order_acquire);
                    if (terminal_state(state) &&
                        mailbox.slots[index].state.compare_exchange_strong(
                            state,
                            SlotState::writing,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed)) {
                        slot_index = index;
                        break;
                    }
                }
            }
            mailbox.reclaiming.store(false, std::memory_order_release);
        }
    }
    if (slot_index == mailbox.registration.record_capacity) {
        count_result(mailbox, LiveControlAdmissionResult::full);
        return LiveControlAdmissionResult::full;
    }

    const auto stride = static_cast<std::size_t>(
        mailbox.registration.payload_bytes_per_record);
    auto* destination = mailbox.payload_storage.get() + slot_index * stride;
    std::copy(payload.begin(), payload.end(), destination);
    std::fill(
        destination + static_cast<std::ptrdiff_t>(payload.size()),
        destination + static_cast<std::ptrdiff_t>(stride),
        std::byte{0});
    auto committed = update;
    committed.mailbox_sequence = mailbox_sequence;
    mailbox.slots[slot_index].record = committed;
    mailbox.slots[slot_index].terminal_generation.store(
        0, std::memory_order_relaxed);
    mailbox.occupancy.fetch_add(1, std::memory_order_relaxed);
    mailbox.slots[slot_index].state.store(
        SlotState::staged, std::memory_order_release);
    auto admission_result = LiveControlAdmissionResult::accepted;
    if (!impl_->admission_open.load(std::memory_order_acquire)) {
        auto expected_state = SlotState::staged;
        if (mailbox.slots[slot_index].state.compare_exchange_strong(
                expected_state,
                SlotState::stopped,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
            increment(impl_->stopped);
            admission_result = LiveControlAdmissionResult::stopped;
        }
    } else if (target_closed(*impl_, committed)) {
        auto expected_state = SlotState::staged;
        if (mailbox.slots[slot_index].state.compare_exchange_strong(
                expected_state,
                SlotState::missed,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
            increment(impl_->missed);
            admission_result = LiveControlAdmissionResult::missed;
        }
    }
    mailbox.next_sequence.store(mailbox_sequence + 1, std::memory_order_release);
    producer.next_sequence.store(
        expected_producer_sequence + 1, std::memory_order_release);
    count_result(mailbox, admission_result);
    return admission_result;
}

namespace {

[[nodiscard]] const LiveControlGenerationView* active_view(
    const LiveControlMailboxSet::Impl& impl) noexcept {
    if (!impl.has_generation.load(std::memory_order_acquire)) {
        return nullptr;
    }
    const auto index = impl.active_generation.load(std::memory_order_acquire);
    return &impl.generations[index].view;
}

[[nodiscard]] const LiveControlGenerationView* close_boundary(
    LiveControlMailboxSet::Impl& impl,
    const LiveControlBoundaryTarget& target) noexcept {
    using SlotState = LiveControlMailboxSet::Impl::SlotState;
    impl.inspection_version.fetch_add(1, std::memory_order_acq_rel);
    std::size_t candidate_count = 0;

    for (std::size_t mailbox_index = 0;
         mailbox_index < impl.mailboxes.size(); ++mailbox_index) {
        auto& mailbox = *impl.mailboxes[mailbox_index];
        for (std::size_t slot_index = 0;
             slot_index < mailbox.registration.record_capacity; ++slot_index) {
            auto& slot = mailbox.slots[slot_index];
            auto state = slot.state.load(std::memory_order_acquire);
            if (state != SlotState::staged) {
                continue;
            }
            const auto& record = slot.record;
            const bool current = target.kind == LiveControlTargetKind::host_frame
                ? record.target_kind == LiveControlTargetKind::host_frame &&
                    record.target_frame_index == target.frame_index
                : record.target_kind == LiveControlTargetKind::rate_release &&
                    record.reference_release_index ==
                        target.reference_release_index &&
                    record.rate_release_sequence ==
                        target.rate_release_sequence;
            if (!current && !target_closed(impl, record)) {
                continue;
            }
            if (!slot.state.compare_exchange_strong(
                    state,
                    SlotState::boundary_owned,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                continue;
            }
            if (!current) {
                slot.terminal_generation.store(0, std::memory_order_relaxed);
                slot.state.store(SlotState::missed, std::memory_order_release);
                mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
                increment(impl.missed);
                continue;
            }
            impl.candidates[candidate_count++] = {
                static_cast<std::uint32_t>(mailbox_index),
                static_cast<std::uint32_t>(slot_index),
                mailbox.registration.mailbox_identity,
                record.mailbox_sequence,
            };
        }
    }

    std::sort(
        impl.candidates.get(),
        impl.candidates.get() + candidate_count,
        [](const auto& left, const auto& right) noexcept {
            if (left.mailbox_identity != right.mailbox_identity) {
                return left.mailbox_identity < right.mailbox_identity;
            }
            return left.mailbox_sequence < right.mailbox_sequence;
        });

    std::array<std::size_t, 5> last_by_kind{};
    last_by_kind.fill(std::numeric_limits<std::size_t>::max());
    std::uint64_t current_mailbox = 0;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto& candidate = impl.candidates[index];
        if (index == 0 || candidate.mailbox_identity != current_mailbox) {
            current_mailbox = candidate.mailbox_identity;
            last_by_kind.fill(std::numeric_limits<std::size_t>::max());
        }
        const auto& record = impl.mailboxes[candidate.mailbox_index]
                                 ->slots[candidate.slot_index]
                                 .record;
        const auto kind_index = static_cast<std::size_t>(record.update_kind) - 1;
        const auto previous = last_by_kind[kind_index];
        if (previous != std::numeric_limits<std::size_t>::max()) {
            impl.candidates[previous].survivor = false;
        }
        last_by_kind[kind_index] = index;
    }

    std::size_t survivor_count = 0;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto& candidate = impl.candidates[index];
        if (candidate.survivor) {
            ++survivor_count;
        }
    }
    if (survivor_count == 0) {
        impl.inspection_version.fetch_add(1, std::memory_order_release);
        return active_view(impl);
    }

    const auto active = impl.active_generation.load(std::memory_order_relaxed);
    const auto inactive = static_cast<std::uint8_t>(active ^ 1u);
    auto& generation = impl.generations[inactive];
    std::uint64_t identity = kFnvOffset;
    hash_target(identity, target);
    std::size_t output_index = 0;
    std::size_t payload_offset = 0;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto& candidate = impl.candidates[index];
        auto& mailbox = *impl.mailboxes[candidate.mailbox_index];
        auto& slot = mailbox.slots[candidate.slot_index];
        if (!candidate.survivor) {
            continue;
        }
        const auto& record = slot.record;
        const auto stride = static_cast<std::size_t>(
            mailbox.registration.payload_bytes_per_record);
        const auto* source = mailbox.payload_storage.get() +
            static_cast<std::size_t>(candidate.slot_index) * stride;
        std::copy_n(
            source,
            record.payload_bytes,
            generation.payloads.get() + payload_offset);
        auto& view = generation.records[output_index++];
        view.record = record;
        view.payload = std::span<const std::byte>(
            generation.payloads.get() + payload_offset,
            record.payload_bytes);
        payload_offset += record.payload_bytes;
        hash_u64(identity, record.mailbox_identity);
        hash_u64(identity, record.mailbox_sequence);
        hash_u64(identity, record.producer_identity);
        hash_u64(identity, record.producer_sequence);
        hash_u64(identity, static_cast<std::uint8_t>(record.update_kind));
        hash_u64(identity, record.payload_digest);
    }
    if (identity == 0) {
        identity = 1;
    }
    generation.view.runtime_id = impl.runtime_id;
    generation.view.configuration_generation = impl.configuration_generation;
    generation.view.generation_identity = identity;
    generation.view.target = target;
    generation.view.records = std::span<const LiveControlRecordView>(
        generation.records.get(), survivor_count);

    impl.active_generation.store(inactive, std::memory_order_release);
    impl.has_generation.store(true, std::memory_order_release);

    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto& candidate = impl.candidates[index];
        auto& mailbox = *impl.mailboxes[candidate.mailbox_index];
        auto& slot = mailbox.slots[candidate.slot_index];
        if (candidate.survivor) {
            slot.terminal_generation.store(identity, std::memory_order_relaxed);
            slot.state.store(SlotState::committed, std::memory_order_release);
            mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
            increment(impl.committed);
        } else {
            slot.terminal_generation.store(identity, std::memory_order_relaxed);
            slot.state.store(SlotState::replaced, std::memory_order_release);
            mailbox.occupancy.fetch_sub(1, std::memory_order_relaxed);
            increment(impl.replaced);
        }
    }

    impl.latest_generation_identity.store(identity, std::memory_order_relaxed);
    impl.latest_frame_index.store(target.frame_index, std::memory_order_relaxed);
    impl.latest_rate_sequence.store(
        target.rate_release_sequence, std::memory_order_relaxed);
    impl.latest_reference_index.store(
        target.reference_release_index, std::memory_order_relaxed);
    impl.latest_domain_index.store(
        target.rate_domain_registration_index, std::memory_order_relaxed);
    impl.latest_phase_index.store(target.phase_index, std::memory_order_relaxed);
    impl.latest_substep.store(
        target.rate_substep_ordinal, std::memory_order_relaxed);
    impl.latest_survivor_count.store(
        static_cast<std::uint32_t>(survivor_count),
        std::memory_order_relaxed);
    impl.latest_target_kind.store(target.kind, std::memory_order_relaxed);
    impl.inspection_version.fetch_add(1, std::memory_order_release);
    return &generation.view;
}

} // namespace

const LiveControlGenerationView* LiveControlMailboxSet::close_host_frame(
    std::uint64_t frame_index) noexcept {
    if (!impl_ || !impl_->admission_open.load(std::memory_order_acquire)) {
        return impl_ ? active_view(*impl_) : nullptr;
    }
    if (impl_->host_boundary_started.load(std::memory_order_acquire) &&
        frame_index <= impl_->last_host_boundary.load(std::memory_order_acquire)) {
        return active_view(*impl_);
    }
    impl_->last_host_boundary.store(frame_index, std::memory_order_release);
    impl_->host_boundary_started.store(true, std::memory_order_release);
    LiveControlBoundaryTarget target;
    target.kind = LiveControlTargetKind::host_frame;
    target.frame_index = frame_index;
    return close_boundary(*impl_, target);
}

const LiveControlGenerationView* LiveControlMailboxSet::close_rate_release(
    std::size_t reference_release_index,
    std::uint64_t release_sequence) noexcept {
    if (!impl_ || reference_release_index >= impl_->rate_target_count ||
        !impl_->admission_open.load(std::memory_order_acquire)) {
        return impl_ ? active_view(*impl_) : nullptr;
    }
    auto& current = impl_->rate_targets[reference_release_index];
    for (std::size_t index = 0; index < impl_->rate_target_count; ++index) {
        if (impl_->rate_targets[index].release_time_ns <
            current.release_time_ns) {
            impl_->rate_targets[index].closed.store(
                true, std::memory_order_release);
        }
    }
    if (current.closed.exchange(true, std::memory_order_acq_rel)) {
        return active_view(*impl_);
    }
    LiveControlBoundaryTarget target;
    target.kind = LiveControlTargetKind::rate_release;
    target.frame_index = kInvalidSequence;
    target.rate_release_sequence = release_sequence;
    target.reference_release_index =
        static_cast<std::uint32_t>(reference_release_index);
    target.rate_domain_registration_index = current.domain_registration_index;
    target.phase_index = current.phase_index;
    target.rate_substep_ordinal = current.substep_ordinal;
    return close_boundary(*impl_, target);
}

const LiveControlGenerationView*
LiveControlMailboxSet::active_generation_view() const noexcept {
    return impl_ ? active_view(*impl_) : nullptr;
}

void LiveControlMailboxSet::expire_rate_releases_before(
    std::uint64_t logical_time_ns) noexcept {
    if (!impl_ || !impl_->admission_open.load(std::memory_order_acquire)) {
        return;
    }
    bool changed = false;
    for (std::size_t index = 0; index < impl_->rate_target_count; ++index) {
        if (impl_->rate_targets[index].release_time_ns < logical_time_ns &&
            !impl_->rate_targets[index].closed.exchange(
                true, std::memory_order_acq_rel)) {
            changed = true;
        }
    }
    if (changed) {
        LiveControlBoundaryTarget no_current_target;
        no_current_target.kind = LiveControlTargetKind::rate_release;
        no_current_target.frame_index = kInvalidSequence;
        (void)close_boundary(*impl_, no_current_target);
    }
}

bool LiveControlMailboxSet::mailbox_info(
    std::uint64_t mailbox_identity,
    LiveControlMailboxInfo& info) const noexcept {
    if (!impl_) {
        return false;
    }
    const auto index = find_mailbox(*impl_, mailbox_identity);
    if (index == impl_->mailboxes.size()) {
        return false;
    }
    const auto& mailbox = *impl_->mailboxes[index];
    LiveControlMailboxInfo candidate;
    candidate.runtime_id = impl_->runtime_id;
    candidate.configuration_generation = impl_->configuration_generation;
    candidate.policy_identity = impl_->policy.policy_identity;
    candidate.mailbox_identity = mailbox_identity;
    candidate.next_mailbox_sequence =
        mailbox.next_sequence.load(std::memory_order_acquire);
    candidate.accepted = mailbox.accepted.load(std::memory_order_acquire);
    candidate.invalid = mailbox.invalid.load(std::memory_order_acquire);
    candidate.full = mailbox.full.load(std::memory_order_acquire);
    candidate.busy = mailbox.busy.load(std::memory_order_acquire);
    candidate.stale = mailbox.stale.load(std::memory_order_acquire);
    candidate.stopped = mailbox.stopped.load(std::memory_order_acquire);
    candidate.exhausted = mailbox.exhausted.load(std::memory_order_acquire);
    candidate.record_capacity = mailbox.registration.record_capacity;
    candidate.payload_bytes_per_record =
        mailbox.registration.payload_bytes_per_record;
    candidate.occupancy = mailbox.occupancy.load(std::memory_order_acquire);
    candidate.producer_count = mailbox.producer_count;
    candidate.admission_open =
        impl_->admission_open.load(std::memory_order_acquire) ? 1u : 0u;
    info = candidate;
    return true;
}

bool LiveControlMailboxSet::record_at(
    std::uint64_t mailbox_identity,
    std::uint64_t mailbox_sequence,
    LiveControlUpdateRecord& record) const noexcept {
    if (!impl_ || mailbox_sequence == 0) {
        return false;
    }
    const auto index = find_mailbox(*impl_, mailbox_identity);
    if (index == impl_->mailboxes.size()) {
        return false;
    }
    auto& mailbox = *impl_->mailboxes[index];
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        return false;
    }
    mailbox.inspection_readers.fetch_add(1, std::memory_order_acq_rel);
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
        return false;
    }
    bool found = false;
    LiveControlUpdateRecord candidate;
    for (std::size_t slot_index = 0;
         slot_index < mailbox.registration.record_capacity; ++slot_index) {
        const auto state = mailbox.slots[slot_index].state.load(
            std::memory_order_acquire);
        if (state == Impl::SlotState::free ||
            state == Impl::SlotState::writing) {
            continue;
        }
        if (mailbox.slots[slot_index].record.mailbox_sequence ==
            mailbox_sequence) {
            candidate = mailbox.slots[slot_index].record;
            found = true;
            break;
        }
    }
    mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
    if (found) {
        record = candidate;
    }
    return found;
}

Status LiveControlMailboxSet::copy_payload(
    std::uint64_t mailbox_identity,
    std::uint64_t mailbox_sequence,
    std::span<std::byte> output) const noexcept {
    if (!impl_ || mailbox_sequence == 0) {
        return Status::invalid_argument;
    }
    const auto mailbox_index = find_mailbox(*impl_, mailbox_identity);
    if (mailbox_index == impl_->mailboxes.size()) {
        return Status::invalid_argument;
    }
    auto& mailbox = *impl_->mailboxes[mailbox_index];
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        return Status::invalid_argument;
    }
    mailbox.inspection_readers.fetch_add(1, std::memory_order_acq_rel);
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
        return Status::invalid_argument;
    }
    auto status = Status::invalid_argument;
    for (std::size_t slot_index = 0;
         slot_index < mailbox.registration.record_capacity; ++slot_index) {
        const auto state = mailbox.slots[slot_index].state.load(
            std::memory_order_acquire);
        if (state == Impl::SlotState::free ||
            state == Impl::SlotState::writing ||
            mailbox.slots[slot_index].record.mailbox_sequence !=
                mailbox_sequence) {
            continue;
        }
        const auto payload_bytes =
            mailbox.slots[slot_index].record.payload_bytes;
        if (output.size() != payload_bytes) {
            status = Status::capacity_exceeded;
            break;
        }
        const auto stride = static_cast<std::size_t>(
            mailbox.registration.payload_bytes_per_record);
        const auto* source = mailbox.payload_storage.get() +
            slot_index * stride;
        std::copy_n(source, output.size(), output.begin());
        status = Status::ok;
        break;
    }
    mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
    return status;
}

bool LiveControlMailboxSet::commit_info(
    LiveControlCommitInfo& info) const noexcept {
    if (!impl_) {
        return false;
    }
    const auto before = impl_->inspection_version.load(std::memory_order_acquire);
    if ((before & 1u) != 0) {
        return false;
    }
    LiveControlCommitInfo candidate;
    candidate.runtime_id = impl_->runtime_id;
    candidate.configuration_generation = impl_->configuration_generation;
    candidate.generation_identity =
        impl_->latest_generation_identity.load(std::memory_order_relaxed);
    candidate.survivor_count =
        impl_->latest_survivor_count.load(std::memory_order_relaxed);
    candidate.target.kind =
        impl_->latest_target_kind.load(std::memory_order_relaxed);
    candidate.target.frame_index =
        impl_->latest_frame_index.load(std::memory_order_relaxed);
    candidate.target.rate_release_sequence =
        impl_->latest_rate_sequence.load(std::memory_order_relaxed);
    candidate.target.reference_release_index =
        impl_->latest_reference_index.load(std::memory_order_relaxed);
    candidate.target.rate_domain_registration_index =
        impl_->latest_domain_index.load(std::memory_order_relaxed);
    candidate.target.phase_index =
        impl_->latest_phase_index.load(std::memory_order_relaxed);
    candidate.target.rate_substep_ordinal =
        impl_->latest_substep.load(std::memory_order_relaxed);
    candidate.committed = impl_->committed.load(std::memory_order_acquire);
    candidate.replaced = impl_->replaced.load(std::memory_order_acquire);
    candidate.missed = impl_->missed.load(std::memory_order_acquire);
    candidate.stopped = impl_->stopped.load(std::memory_order_acquire);
    std::uint64_t occupancy = 0;
    for (const auto& mailbox : impl_->mailboxes) {
        occupancy += mailbox->occupancy.load(std::memory_order_acquire);
    }
    if (occupancy > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    candidate.staged_occupancy = static_cast<std::uint32_t>(occupancy);
    const auto after = impl_->inspection_version.load(std::memory_order_acquire);
    if (before != after || (after & 1u) != 0) {
        return false;
    }
    info = candidate;
    return true;
}

bool LiveControlMailboxSet::record_status(
    std::uint64_t mailbox_identity,
    std::uint64_t mailbox_sequence,
    LiveControlRecordStatusInfo& info) const noexcept {
    if (!impl_ || mailbox_sequence == 0) {
        return false;
    }
    const auto mailbox_index = find_mailbox(*impl_, mailbox_identity);
    if (mailbox_index == impl_->mailboxes.size()) {
        return false;
    }
    auto& mailbox = *impl_->mailboxes[mailbox_index];
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        return false;
    }
    mailbox.inspection_readers.fetch_add(1, std::memory_order_acq_rel);
    if (mailbox.reclaiming.load(std::memory_order_acquire)) {
        mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
        return false;
    }
    bool found = false;
    LiveControlRecordStatusInfo candidate;
    for (std::size_t index = 0;
         index < mailbox.registration.record_capacity; ++index) {
        const auto state = mailbox.slots[index].state.load(
            std::memory_order_acquire);
        if (state == Impl::SlotState::free ||
            state == Impl::SlotState::writing ||
            mailbox.slots[index].record.mailbox_sequence != mailbox_sequence) {
            continue;
        }
        const auto& record = mailbox.slots[index].record;
        candidate.runtime_id = impl_->runtime_id;
        candidate.configuration_generation = impl_->configuration_generation;
        candidate.mailbox_identity = record.mailbox_identity;
        candidate.mailbox_sequence = record.mailbox_sequence;
        candidate.producer_identity = record.producer_identity;
        candidate.producer_sequence = record.producer_sequence;
        candidate.generation_identity =
            mailbox.slots[index].terminal_generation.load(
                std::memory_order_acquire);
        candidate.status = public_status(state);
        candidate.update_kind = record.update_kind;
        candidate.target_kind = record.target_kind;
        found = true;
        break;
    }
    mailbox.inspection_readers.fetch_sub(1, std::memory_order_release);
    if (found) {
        info = candidate;
    }
    return found;
}

void LiveControlMailboxSet::close_admission() noexcept {
    if (impl_) {
        impl_->admission_open.store(false, std::memory_order_release);
    }
}

void LiveControlMailboxSet::terminalize_staged_on_stop() noexcept {
    if (!impl_) {
        return;
    }
    for (auto& mailbox : impl_->mailboxes) {
        for (std::size_t index = 0;
             index < mailbox->registration.record_capacity; ++index) {
            auto expected = Impl::SlotState::staged;
            if (mailbox->slots[index].state.compare_exchange_strong(
                    expected,
                    Impl::SlotState::stopped,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                mailbox->slots[index].terminal_generation.store(
                    0, std::memory_order_release);
                mailbox->occupancy.fetch_sub(1, std::memory_order_relaxed);
                increment(impl_->stopped);
            }
        }
    }
}

bool LiveControlMailboxSet::has_active_reservation() const noexcept {
    if (!impl_) {
        return false;
    }
    return std::any_of(
        impl_->mailboxes.begin(),
        impl_->mailboxes.end(),
        [](const std::unique_ptr<Impl::Mailbox>& mailbox) {
            return mailbox->reservation.test(std::memory_order_acquire);
        });
}

std::size_t LiveControlMailboxSet::mailbox_count() const noexcept {
    return impl_ ? impl_->mailboxes.size() : 0;
}

std::size_t LiveControlMailboxSet::producer_count() const noexcept {
    return impl_ ? impl_->producer_count : 0;
}

std::size_t LiveControlMailboxSet::record_capacity() const noexcept {
    return impl_ ? impl_->total_record_capacity : 0;
}

std::size_t LiveControlMailboxSet::payload_storage_bytes() const noexcept {
    return impl_ ? impl_->total_payload_storage_bytes : 0;
}

std::size_t LiveControlMailboxSet::control_bytes() const noexcept {
    if (!impl_) {
        return 0;
    }
    std::size_t total = sizeof(LiveControlMailboxSet) + sizeof(Impl);
    (void)checked_add(
        total,
        impl_->mailboxes.capacity() * sizeof(std::unique_ptr<Impl::Mailbox>),
        total);
    for (const auto& mailbox : impl_->mailboxes) {
        (void)checked_add(total, sizeof(Impl::Mailbox), total);
        (void)checked_add(
            total,
            static_cast<std::size_t>(mailbox->registration.record_capacity) *
                sizeof(Impl::Slot),
            total);
        (void)checked_add(
            total,
            static_cast<std::size_t>(mailbox->registration.record_capacity) *
                mailbox->registration.payload_bytes_per_record,
            total);
    }
    (void)checked_add(
        total, impl_->producer_count * sizeof(Impl::Producer), total);
    (void)checked_add(
        total, impl_->rate_target_count * sizeof(Impl::RateTarget), total);
    (void)checked_add(
        total, impl_->total_record_capacity * sizeof(Impl::Candidate), total);
    for (const auto& generation : impl_->generations) {
        (void)generation;
        (void)checked_add(
            total,
            impl_->total_record_capacity * sizeof(LiveControlRecordView),
            total);
        (void)checked_add(
            total, impl_->total_payload_storage_bytes, total);
    }
    return total;
}

std::size_t LiveControlMailboxSet::extent_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    std::size_t count = 2;
    count += impl_->mailboxes.capacity() != 0 ? 1u : 0u;
    count += impl_->mailboxes.size() * 3u;
    count += impl_->producer_count != 0 ? 1u : 0u;
    count += impl_->rate_target_count != 0 ? 1u : 0u;
    count += impl_->total_record_capacity != 0 ? 1u : 0u;
    count += impl_->total_record_capacity != 0 ? 2u : 0u;
    count += impl_->total_payload_storage_bytes != 0 ? 2u : 0u;
    return count;
}

bool LiveControlMailboxSet::extent_at(
    std::size_t requested,
    LiveControlStorageExtent& extent) const noexcept {
    extent = {};
    if (!impl_) {
        return false;
    }
    std::size_t current = 0;
    const auto emit = [&](const void* data, std::size_t bytes) {
        if (bytes == 0) {
            return false;
        }
        if (current++ == requested) {
            extent = {data, bytes};
            return true;
        }
        return false;
    };
    if (emit(this, sizeof(LiveControlMailboxSet)) ||
        emit(impl_.get(), sizeof(Impl)) ||
        emit(impl_->mailboxes.data(),
             impl_->mailboxes.capacity() *
                 sizeof(std::unique_ptr<Impl::Mailbox>))) {
        return true;
    }
    for (const auto& mailbox : impl_->mailboxes) {
        if (emit(mailbox.get(), sizeof(Impl::Mailbox)) ||
            emit(mailbox->slots.get(),
                 static_cast<std::size_t>(mailbox->registration.record_capacity) *
                     sizeof(Impl::Slot)) ||
            emit(mailbox->payload_storage.get(),
                 static_cast<std::size_t>(mailbox->registration.record_capacity) *
                     mailbox->registration.payload_bytes_per_record)) {
            return true;
        }
    }
    if (emit(impl_->producers.get(),
             impl_->producer_count * sizeof(Impl::Producer)) ||
        emit(impl_->rate_targets.get(),
             impl_->rate_target_count * sizeof(Impl::RateTarget)) ||
        emit(impl_->candidates.get(),
             impl_->total_record_capacity * sizeof(Impl::Candidate))) {
        return true;
    }
    for (const auto& generation : impl_->generations) {
        if (emit(generation.records.get(),
                 impl_->total_record_capacity *
                     sizeof(LiveControlRecordView)) ||
            emit(generation.payloads.get(),
                 impl_->total_payload_storage_bytes)) {
            return true;
        }
    }
    return false;
}

std::uint64_t LiveControlMailboxSet::runtime_id() const noexcept {
    return impl_ ? impl_->runtime_id : 0;
}

std::uint64_t LiveControlMailboxSet::configuration_generation() const noexcept {
    return impl_ ? impl_->configuration_generation : 0;
}

bool LiveControlMailboxSet::claim_for_test(
    std::uint64_t mailbox_identity) noexcept {
    if (!impl_) {
        return false;
    }
    const auto index = find_mailbox(*impl_, mailbox_identity);
    return index != impl_->mailboxes.size() &&
        !impl_->mailboxes[index]->reservation.test_and_set(
            std::memory_order_acquire);
}

void LiveControlMailboxSet::release_for_test(
    std::uint64_t mailbox_identity) noexcept {
    if (!impl_) {
        return;
    }
    const auto index = find_mailbox(*impl_, mailbox_identity);
    if (index != impl_->mailboxes.size()) {
        impl_->mailboxes[index]->reservation.clear(std::memory_order_release);
    }
}

} // namespace rt::detail

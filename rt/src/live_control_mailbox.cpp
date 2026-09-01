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
    struct RateTarget {
        std::uint64_t release_sequence = 0;
        std::uint32_t domain_registration_index = 0;
        std::uint32_t phase_index = 0;
        std::uint32_t substep_ordinal = 0;
    };

    struct Slot {
        std::atomic<std::uint8_t> published{0};
        LiveControlUpdateRecord record{};
    };

    struct Mailbox {
        LiveControlMailboxRegistration registration{};
        std::uint32_t producer_count = 0;
        std::atomic_flag reservation = ATOMIC_FLAG_INIT;
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
    std::vector<RateTarget> rate_targets{};
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
        update.reference_release_index >= impl.rate_targets.size()) {
        return false;
    }
    const auto& target = impl.rate_targets[update.reference_release_index];
    return update.rate_domain_registration_index ==
               target.domain_registration_index &&
        update.phase_index == target.phase_index &&
        update.rate_substep_ordinal == target.substep_ordinal &&
        update.rate_release_sequence == target.release_sequence;
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
    }
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
        impl->rate_targets.reserve(rate_releases.size());
        for (const auto& release : rate_releases) {
            if (release.domain_registration_index > kInvalidIndex) {
                diagnostic = "live-control rate target identity overflows";
                return Status::capacity_exceeded;
            }
            impl->rate_targets.push_back({
                release.domain_release_sequence,
                static_cast<std::uint32_t>(release.domain_registration_index),
                release.phase.index(),
                release.substep_ordinal,
            });
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
    const auto occupancy = mailbox.occupancy.load(std::memory_order_relaxed);
    if (occupancy >= mailbox.registration.record_capacity) {
        count_result(mailbox, LiveControlAdmissionResult::full);
        return LiveControlAdmissionResult::full;
    }
    const auto mailbox_sequence =
        mailbox.next_sequence.load(std::memory_order_relaxed);
    if (mailbox_sequence == kInvalidSequence) {
        count_result(mailbox, LiveControlAdmissionResult::exhausted);
        return LiveControlAdmissionResult::exhausted;
    }

    const auto slot_index = static_cast<std::size_t>(occupancy);
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
    mailbox.slots[slot_index].published.store(1, std::memory_order_release);
    mailbox.occupancy.store(occupancy + 1, std::memory_order_release);
    mailbox.next_sequence.store(mailbox_sequence + 1, std::memory_order_release);
    producer.next_sequence.store(
        expected_producer_sequence + 1, std::memory_order_release);
    count_result(mailbox, LiveControlAdmissionResult::accepted);
    return LiveControlAdmissionResult::accepted;
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
    const auto& mailbox = *impl_->mailboxes[index];
    const auto slot_index = mailbox_sequence - 1;
    if (slot_index >= mailbox.registration.record_capacity ||
        mailbox.slots[slot_index].published.load(std::memory_order_acquire) == 0) {
        return false;
    }
    const auto candidate = mailbox.slots[slot_index].record;
    if (candidate.mailbox_sequence != mailbox_sequence) {
        return false;
    }
    record = candidate;
    return true;
}

Status LiveControlMailboxSet::copy_payload(
    std::uint64_t mailbox_identity,
    std::uint64_t mailbox_sequence,
    std::span<std::byte> output) const noexcept {
    LiveControlUpdateRecord record;
    if (!record_at(mailbox_identity, mailbox_sequence, record)) {
        return Status::invalid_argument;
    }
    if (output.size() != record.payload_bytes) {
        return Status::capacity_exceeded;
    }
    const auto mailbox_index = find_mailbox(*impl_, mailbox_identity);
    const auto& mailbox = *impl_->mailboxes[mailbox_index];
    const auto slot_index = static_cast<std::size_t>(mailbox_sequence - 1);
    const auto stride = static_cast<std::size_t>(
        mailbox.registration.payload_bytes_per_record);
    const auto* source = mailbox.payload_storage.get() + slot_index * stride;
    std::copy_n(source, output.size(), output.begin());
    return Status::ok;
}

void LiveControlMailboxSet::close_admission() noexcept {
    if (impl_) {
        impl_->admission_open.store(false, std::memory_order_release);
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
        total, impl_->rate_targets.capacity() * sizeof(Impl::RateTarget), total);
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
    count += impl_->rate_targets.capacity() != 0 ? 1u : 0u;
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
    return emit(impl_->producers.get(),
                impl_->producer_count * sizeof(Impl::Producer)) ||
        emit(impl_->rate_targets.data(),
             impl_->rate_targets.capacity() * sizeof(Impl::RateTarget));
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

#include "hal_v2.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace {

template <typename Range>
bool all_zero(const Range& values) noexcept {
    return std::all_of(
        values.begin(), values.end(), [](const auto value) {
            return value == 0;
        });
}

bool identifier_valid(const char* value, std::size_t capacity) noexcept {
    if (!value || capacity == 0 || value[0] == '\0') {
        return false;
    }
    const auto* end = static_cast<const char*>(
        std::memchr(value, '\0', capacity));
    if (!end) {
        return false;
    }
    for (const auto* current = value; current != end; ++current) {
        const auto byte = static_cast<unsigned char>(*current);
        const bool valid =
            (byte >= static_cast<unsigned char>('a') &&
             byte <= static_cast<unsigned char>('z')) ||
            (byte >= static_cast<unsigned char>('A') &&
             byte <= static_cast<unsigned char>('Z')) ||
            (byte >= static_cast<unsigned char>('0') &&
             byte <= static_cast<unsigned char>('9')) ||
            byte == static_cast<unsigned char>('.') ||
            byte == static_cast<unsigned char>('_') ||
            byte == static_cast<unsigned char>(':') ||
            byte == static_cast<unsigned char>('/') ||
            byte == static_cast<unsigned char>('@') ||
            byte == static_cast<unsigned char>('-');
        if (!valid) {
            return false;
        }
    }
    return std::all_of(
        end + 1,
        value + capacity,
        [](char byte) { return byte == '\0'; });
}

bool valid_flags(std::uint32_t flags) noexcept {
    constexpr auto allowed =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    return flags != 0 && (flags & ~allowed) == 0;
}

bool valid_access(std::uint32_t access) noexcept {
    return access == RTFW_DEVICE_ACCESS_READ ||
           access == RTFW_DEVICE_ACCESS_WRITE ||
           access == RTFW_DEVICE_ACCESS_READ_WRITE;
}

bool valid_v1_status(rtfw_device_status status) noexcept {
    return status <= RTFW_DEVICE_STATUS_OK &&
           status >= RTFW_DEVICE_STATUS_RESET_REQUIRED;
}

bool valid_hal_status(std::int32_t status) noexcept {
    return status <= static_cast<std::int32_t>(rt::HalV2Status::ok) &&
           status >=
               static_cast<std::int32_t>(rt::HalV2Status::reset_required);
}

rt::HalV2Status v1_status_to_hal(rtfw_device_status status) noexcept {
    if (!valid_v1_status(status)) {
        return rt::HalV2Status::internal_error;
    }
    return static_cast<rt::HalV2Status>(status);
}

bool valid_v1_capabilities(
    const rtfw_device_capabilities& capabilities) noexcept {
    const auto reserved_zero = std::all_of(
        std::begin(capabilities.reserved),
        std::end(capabilities.reserved),
        [](std::uint64_t value) { return value == 0; });
    return capabilities.struct_size >= sizeof(capabilities) &&
           capabilities.abi_version == RTFW_DEVICE_ABI_VERSION &&
           capabilities.max_in_flight != 0 &&
           capabilities.max_registered_buffers != 0 &&
           capabilities.max_buffer_bytes != 0 &&
           capabilities.inline_payload_capacity >=
               RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY &&
           capabilities.buffer_ref_capacity >=
               RTFW_DEVICE_BUFFER_REF_CAPACITY &&
           capabilities.supports_cancel <= 1 &&
           capabilities.supports_reset <= 1 &&
           capabilities.deterministic_mock <= 1 &&
           capabilities.reserved0 == 0 &&
           identifier_valid(
               capabilities.backend_id,
               RTFW_DEVICE_IDENTIFIER_CAPACITY) &&
           reserved_zero &&
           capabilities.control_storage_bytes <=
               std::numeric_limits<std::size_t>::max();
}

bool valid_v1_completion(
    const rtfw_device_completion& completion) noexcept {
    return completion.struct_size >= sizeof(completion) &&
           valid_v1_status(completion.status) &&
           completion.submission_id != 0 &&
           std::all_of(
               std::begin(completion.reserved),
               std::end(completion.reserved),
               [](std::uint64_t value) { return value == 0; });
}

bool valid_v1_health(const rtfw_device_health& health) noexcept {
    return health.struct_size >= sizeof(health) &&
           health.state <= RTFW_DEVICE_HEALTH_LOST &&
           valid_v1_status(health.last_status) &&
           health.reserved0 == 0 &&
           std::all_of(
               std::begin(health.reserved),
               std::end(health.reserved),
               [](std::uint64_t value) { return value == 0; });
}

} // namespace

namespace rt::detail {

bool validate_device_v1_api(const rtfw_device_backend_api& api) noexcept {
    return api.struct_size >= sizeof(api) &&
           api.abi_version == RTFW_DEVICE_ABI_VERSION && api.instance &&
           api.get_capabilities && api.initialize && api.register_buffer &&
           api.unregister_buffer && api.submit && api.poll && api.cancel &&
           api.get_health && api.reset && api.shutdown &&
           std::all_of(
               std::begin(api.reserved),
               std::end(api.reserved),
               [](std::uint64_t value) { return value == 0; });
}

bool validate_hal_v2_api(const HalV2BackendApi& api) noexcept {
    return api.struct_size >= sizeof(api) &&
           api.api_version == hal_v2_api_version && api.instance &&
           api.get_capabilities && api.initialize && api.register_buffer &&
           api.unregister_buffer && api.submit && api.poll && api.cancel &&
           api.get_health && api.reset && api.shutdown &&
           all_zero(api.reserved);
}

bool validate_hal_v2_capabilities(
    const HalV2Capabilities& capabilities) noexcept {
    return capabilities.struct_size >= sizeof(capabilities) &&
           capabilities.api_version == hal_v2_api_version &&
           capabilities.max_in_flight != 0 &&
           capabilities.max_registered_buffers != 0 &&
           capabilities.max_buffer_bytes != 0 &&
           capabilities.inline_payload_capacity >=
               hal_v2_inline_payload_capacity &&
           capabilities.buffer_ref_capacity >=
               hal_v2_buffer_ref_capacity &&
           capabilities.supports_cancel <= 1 &&
           capabilities.supports_reset <= 1 &&
           capabilities.deterministic_mock <= 1 &&
           capabilities.reserved0 == 0 &&
           identifier_valid(
               capabilities.backend_id.data(),
               capabilities.backend_id.size()) &&
           all_zero(capabilities.reserved) &&
           capabilities.control_storage_bytes <=
               std::numeric_limits<std::size_t>::max();
}

bool validate_hal_v2_health(const HalV2Health& health) noexcept {
    return health.struct_size >= sizeof(health) &&
           health.state <=
               static_cast<std::uint32_t>(HalV2HealthState::lost) &&
           valid_hal_status(health.last_status) &&
           health.reserved0 == 0 && all_zero(health.reserved);
}

bool validate_hal_v2_completion(
    const HalV2Completion& completion) noexcept {
    return completion.struct_size >= sizeof(completion) &&
           valid_hal_status(completion.status) &&
           completion.submission_id != 0 &&
           all_zero(completion.reserved);
}

Status hal_v2_status_to_runtime(HalV2Status status) noexcept {
    switch (status) {
    case HalV2Status::ok:
        return Status::ok;
    case HalV2Status::invalid_argument:
        return Status::invalid_argument;
    case HalV2Status::invalid_state:
        return Status::invalid_state;
    case HalV2Status::queue_full:
        return Status::device_queue_full;
    case HalV2Status::timeout:
        return Status::device_timeout;
    case HalV2Status::error:
    case HalV2Status::unsupported:
    case HalV2Status::internal_error:
        return Status::device_error;
    case HalV2Status::lost:
        return Status::device_lost;
    case HalV2Status::canceled:
        return Status::device_canceled;
    case HalV2Status::resource_exhausted:
        return Status::resource_exhausted;
    case HalV2Status::reset_required:
        return Status::device_reset_required;
    }
    return Status::device_error;
}

DeviceV1CompatibilityAdapter::DeviceV1CompatibilityAdapter(
    const rtfw_device_backend_api& api) noexcept
    : v1_(api) {
    api_.instance = this;
    api_.get_capabilities = &get_capabilities;
    api_.initialize = &initialize;
    api_.register_buffer = &register_buffer;
    api_.unregister_buffer = &unregister_buffer;
    api_.submit = &submit;
    api_.poll = &poll;
    api_.cancel = &cancel;
    api_.get_health = &get_health;
    api_.reset = &reset;
    api_.shutdown = &shutdown;
}

Status DeviceV1CompatibilityAdapter::prepare_completion_storage(
    std::size_t capacity) noexcept {
    if (capacity == 0) {
        return Status::invalid_config;
    }
    if (capacity == completion_capacity_ && completion_storage_) {
        return Status::ok;
    }
    try {
        auto candidate = std::make_unique<rtfw_device_completion[]>(capacity);
        completion_storage_ = std::move(candidate);
        completion_capacity_ = capacity;
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::resource_exhausted;
    } catch (...) {
        return Status::internal_error;
    }
}

HalV2Status DeviceV1CompatibilityAdapter::get_capabilities(
    void* instance, HalV2Capabilities* capabilities) {
    auto* self = static_cast<DeviceV1CompatibilityAdapter*>(instance);
    if (!self || !capabilities ||
        capabilities->struct_size < sizeof(*capabilities) ||
        capabilities->api_version != hal_v2_api_version) {
        return HalV2Status::invalid_argument;
    }
    rtfw_device_capabilities v1{};
    v1.struct_size = sizeof(v1);
    v1.abi_version = RTFW_DEVICE_ABI_VERSION;
    rtfw_device_status status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    try {
        status = self->v1_.get_capabilities(self->v1_.instance, &v1);
    } catch (...) {
        return HalV2Status::internal_error;
    }
    if (!valid_v1_status(status)) {
        return HalV2Status::internal_error;
    }
    if (status != RTFW_DEVICE_STATUS_OK) {
        return v1_status_to_hal(status);
    }
    if (!valid_v1_capabilities(v1)) {
        return HalV2Status::invalid_argument;
    }
    HalV2Capabilities output;
    output.max_in_flight = v1.max_in_flight;
    output.max_registered_buffers = v1.max_registered_buffers;
    output.max_buffer_bytes = v1.max_buffer_bytes;
    output.inline_payload_capacity = v1.inline_payload_capacity;
    output.buffer_ref_capacity = v1.buffer_ref_capacity;
    output.supports_cancel = v1.supports_cancel;
    output.supports_reset = v1.supports_reset;
    output.deterministic_mock = v1.deterministic_mock;
    std::copy(
        std::begin(v1.backend_id),
        std::end(v1.backend_id),
        output.backend_id.begin());
    output.control_storage_bytes = v1.control_storage_bytes;
    *capabilities = output;
    return HalV2Status::ok;
}

HalV2Status DeviceV1CompatibilityAdapter::initialize(
    void* instance, const HalV2InitializeConfig* config) {
    auto* self = static_cast<DeviceV1CompatibilityAdapter*>(instance);
    if (!self || !config || config->struct_size < sizeof(*config) ||
        config->api_version != hal_v2_api_version ||
        config->requested_in_flight == 0 ||
        !all_zero(config->reserved)) {
        return HalV2Status::invalid_argument;
    }
    rtfw_device_init_config v1{};
    v1.struct_size = sizeof(v1);
    v1.abi_version = RTFW_DEVICE_ABI_VERSION;
    v1.requested_in_flight = config->requested_in_flight;
    v1.requested_registered_buffers =
        config->requested_registered_buffers;
    try {
        return v1_status_to_hal(
            self->v1_.initialize(self->v1_.instance, &v1));
    } catch (...) {
        return HalV2Status::internal_error;
    }
}

HalV2Status DeviceV1CompatibilityAdapter::register_buffer(
    void* instance,
    const HalV2BufferRegistration* registration,
    std::uint64_t* out_buffer_token) {
    auto* self = static_cast<DeviceV1CompatibilityAdapter*>(instance);
    if (out_buffer_token) {
        *out_buffer_token = 0;
    }
    if (!self || !registration || !out_buffer_token ||
        registration->struct_size < sizeof(*registration) ||
        !registration->data || registration->bytes == 0 ||
        !valid_flags(registration->flags) ||
        !identifier_valid(
            registration->name.data(), registration->name.size()) ||
        !all_zero(registration->reserved)) {
        return HalV2Status::invalid_argument;
    }
    rtfw_device_buffer_registration v1{};
    v1.struct_size = sizeof(v1);
    v1.flags = registration->flags;
    v1.data = registration->data;
    v1.bytes = registration->bytes;
    std::copy(registration->name.begin(), registration->name.end(), v1.name);
    std::uint64_t token = 0;
    rtfw_device_status status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    try {
        status = self->v1_.register_buffer(
            self->v1_.instance, &v1, &token);
    } catch (...) {
        if (token != 0) {
            *out_buffer_token = token;
        }
        return HalV2Status::internal_error;
    }
    if (token != 0) {
        *out_buffer_token = token;
    }
    if (!valid_v1_status(status)) {
        return HalV2Status::internal_error;
    }
    if (status != RTFW_DEVICE_STATUS_OK) {
        return v1_status_to_hal(status);
    }
    if (token == 0) {
        return HalV2Status::internal_error;
    }
    return HalV2Status::ok;
}

HalV2Status DeviceV1CompatibilityAdapter::unregister_buffer(
    void* instance, std::uint64_t buffer_token) {
    auto* self = static_cast<DeviceV1CompatibilityAdapter*>(instance);
    if (!self || buffer_token == 0) {
        return HalV2Status::invalid_argument;
    }
    try {
        return v1_status_to_hal(self->v1_.unregister_buffer(
            self->v1_.instance, buffer_token));
    } catch (...) {
        return HalV2Status::internal_error;
    }
}

HalV2Status DeviceV1CompatibilityAdapter::submit(
    void* instance, const HalV2Submission* submission) {
    auto* self = static_cast<DeviceV1CompatibilityAdapter*>(instance);
    if (!self || !submission ||
        submission->struct_size < sizeof(*submission) ||
        submission->api_version != hal_v2_api_version ||
        submission->submission_id == 0 || submission->timeout_ns == 0 ||
        submission->flags != 0 ||
        submission->payload_size > hal_v2_inline_payload_capacity ||
        submission->buffer_count > hal_v2_buffer_ref_capacity ||
        !all_zero(submission->reserved)) {
        return HalV2Status::invalid_argument;
    }
    rtfw_device_submission v1{};
    v1.struct_size = sizeof(v1);
    v1.abi_version = RTFW_DEVICE_ABI_VERSION;
    v1.submission_id = submission->submission_id;
    v1.frame_index = submission->frame_index;
    v1.timeout_ns = submission->timeout_ns;
    v1.opcode = submission->opcode;
    v1.flags = submission->flags;
    v1.payload_size = submission->payload_size;
    v1.buffer_count = submission->buffer_count;
    std::copy(submission->payload.begin(), submission->payload.end(), v1.payload);
    for (std::size_t index = 0; index < submission->buffer_count; ++index) {
        const auto& source = submission->buffers[index];
        if (source.buffer_token == 0 || source.reserved0 != 0 ||
            !valid_access(source.access)) {
            return HalV2Status::invalid_argument;
        }
        auto& target = v1.buffers[index];
        target.buffer_token = source.buffer_token;
        target.access = source.access;
        target.offset = source.offset;
        target.bytes = source.bytes;
    }
    try {
        return v1_status_to_hal(
            self->v1_.submit(self->v1_.instance, &v1));
    } catch (...) {
        return HalV2Status::internal_error;
    }
}

HalV2Status DeviceV1CompatibilityAdapter::poll(
    void* instance,
    HalV2Completion* completions,
    std::uint64_t completion_capacity,
    std::uint64_t* out_completion_count) {
    auto* self = static_cast<DeviceV1CompatibilityAdapter*>(instance);
    if (out_completion_count) {
        *out_completion_count = 0;
    }
    if (!self || !completions || !out_completion_count ||
        completion_capacity == 0 ||
        completion_capacity > self->completion_capacity_ ||
        completion_capacity > std::numeric_limits<std::size_t>::max() ||
        !self->completion_storage_) {
        return HalV2Status::invalid_argument;
    }
    const auto capacity = static_cast<std::size_t>(completion_capacity);
    std::fill_n(
        self->completion_storage_.get(),
        capacity,
        rtfw_device_completion{});
    std::uint64_t count = 0;
    rtfw_device_status status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    try {
        status = self->v1_.poll(
            self->v1_.instance,
            self->completion_storage_.get(),
            completion_capacity,
            &count);
    } catch (...) {
        return HalV2Status::internal_error;
    }
    if (!valid_v1_status(status)) {
        return HalV2Status::internal_error;
    }
    if (status != RTFW_DEVICE_STATUS_OK) {
        return v1_status_to_hal(status);
    }
    if (count > completion_capacity) {
        return HalV2Status::internal_error;
    }
    for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
        if (!valid_v1_completion(self->completion_storage_[index])) {
            return HalV2Status::internal_error;
        }
    }
    for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
        const auto& source = self->completion_storage_[index];
        HalV2Completion target;
        target.status = source.status;
        target.submission_id = source.submission_id;
        target.device_timestamp_ns = source.device_timestamp_ns;
        target.value = source.value;
        completions[index] = target;
    }
    *out_completion_count = count;
    return HalV2Status::ok;
}

HalV2Status DeviceV1CompatibilityAdapter::cancel(
    void* instance, std::uint64_t submission_id) {
    auto* self = static_cast<DeviceV1CompatibilityAdapter*>(instance);
    if (!self || submission_id == 0) {
        return HalV2Status::invalid_argument;
    }
    try {
        return v1_status_to_hal(
            self->v1_.cancel(self->v1_.instance, submission_id));
    } catch (...) {
        return HalV2Status::internal_error;
    }
}

HalV2Status DeviceV1CompatibilityAdapter::get_health(
    void* instance, HalV2Health* health) {
    auto* self = static_cast<DeviceV1CompatibilityAdapter*>(instance);
    if (!self || !health || health->struct_size < sizeof(*health)) {
        return HalV2Status::invalid_argument;
    }
    rtfw_device_health v1{};
    v1.struct_size = sizeof(v1);
    v1.reserved0 = std::numeric_limits<std::uint32_t>::max();
    rtfw_device_status status = RTFW_DEVICE_STATUS_INTERNAL_ERROR;
    try {
        status = self->v1_.get_health(self->v1_.instance, &v1);
    } catch (...) {
        return HalV2Status::internal_error;
    }
    if (!valid_v1_status(status)) {
        return HalV2Status::internal_error;
    }
    if (status != RTFW_DEVICE_STATUS_OK) {
        return v1_status_to_hal(status);
    }
    if (!valid_v1_health(v1)) {
        return HalV2Status::internal_error;
    }
    HalV2Health output;
    output.state = v1.state;
    output.last_status = v1.last_status;
    output.generation = v1.generation;
    output.submissions = v1.submissions;
    output.completions = v1.completions;
    output.queue_rejections = v1.queue_rejections;
    output.timeouts = v1.timeouts;
    output.errors = v1.errors;
    output.losses = v1.losses;
    output.cancellations = v1.cancellations;
    output.resets = v1.resets;
    output.outstanding = v1.outstanding;
    *health = output;
    return HalV2Status::ok;
}

HalV2Status DeviceV1CompatibilityAdapter::reset(void* instance) {
    auto* self = static_cast<DeviceV1CompatibilityAdapter*>(instance);
    if (!self) {
        return HalV2Status::invalid_argument;
    }
    try {
        return v1_status_to_hal(self->v1_.reset(self->v1_.instance));
    } catch (...) {
        return HalV2Status::internal_error;
    }
}

HalV2Status DeviceV1CompatibilityAdapter::shutdown(void* instance) {
    auto* self = static_cast<DeviceV1CompatibilityAdapter*>(instance);
    if (!self) {
        return HalV2Status::invalid_argument;
    }
    try {
        return v1_status_to_hal(self->v1_.shutdown(self->v1_.instance));
    } catch (...) {
        return HalV2Status::internal_error;
    }
}

} // namespace rt::detail

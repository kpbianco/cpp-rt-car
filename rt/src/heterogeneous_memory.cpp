#include "heterogeneous_memory.hpp"
#include "hal_v2.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace {

template <typename Range> bool all_zero(const Range &values) noexcept {
  return std::all_of(values.begin(), values.end(),
                     [](const auto value) { return value == 0; });
}

bool power_of_two(std::uint64_t value) noexcept {
  return value != 0 && (value & (value - 1u)) == 0;
}

bool valid_domain_kind(std::uint32_t value) noexcept {
  return value >= static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::host) &&
         value <= static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::peer);
}

bool valid_node_kind(std::uint32_t value) noexcept {
  return value >= static_cast<std::uint32_t>(rt::HalV2TopologyNodeKind::host) &&
         value <= static_cast<std::uint32_t>(
                      rt::HalV2TopologyNodeKind::peer_endpoint);
}

bool valid_link_kind(std::uint32_t value) noexcept {
  return value >=
             static_cast<std::uint32_t>(rt::HalV2TopologyLinkKind::local) &&
         value <= static_cast<std::uint32_t>(rt::HalV2TopologyLinkKind::peer);
}

bool valid_timestamp_kind(std::uint32_t value) noexcept {
  return value >= static_cast<std::uint32_t>(
                      rt::HalV2TimestampDomainKind::runtime_monotonic) &&
         value <=
             static_cast<std::uint32_t>(rt::HalV2TimestampDomainKind::external);
}

template <typename Array>
bool unique_identity(const Array &values, std::size_t count) noexcept {
  for (std::size_t left = 0; left < count; ++left) {
    if (values[left].identity == 0) {
      return false;
    }
    for (std::size_t right = left + 1; right < count; ++right) {
      if (values[left].identity == values[right].identity) {
        return false;
      }
    }
  }
  return true;
}

template <typename Array>
bool contains_identity(const Array &values, std::size_t count,
                       std::uint64_t identity) noexcept {
  return identity != 0 &&
         std::any_of(values.begin(),
                     values.begin() + static_cast<std::ptrdiff_t>(count),
                     [identity](const auto &value) {
                       return value.identity == identity;
                     });
}

bool domain_consistent(const rt::HalV2MemoryDomain &domain) noexcept {
  constexpr auto ownership_mask = rt::hal_v2_memory_ownership_borrowed_host |
                                  rt::hal_v2_memory_ownership_borrowed_opaque |
                                  rt::hal_v2_memory_ownership_backend;
  if (domain.struct_size < sizeof(domain) ||
      domain.extension_version !=
          rt::hal_v2_memory_topology_extension_version ||
      domain.identity == 0 || !valid_domain_kind(domain.kind) ||
      domain.ownership_modes == 0 ||
      (domain.ownership_modes & ~ownership_mask) != 0 ||
      domain.maximum_bytes == 0 || !power_of_two(domain.byte_granularity) ||
      !power_of_two(domain.alignment) ||
      !power_of_two(domain.offset_granularity) ||
      domain.maximum_bytes < domain.byte_granularity ||
      domain.maximum_bytes % domain.byte_granularity != 0 ||
      !rt::detail::valid_memory_access(domain.access) ||
      !rt::detail::valid_memory_coherency(domain.coherency) ||
      !rt::detail::valid_memory_synchronization(
          domain.required_synchronization) ||
      domain.reserved0 != 0 || !all_zero(domain.reserved)) {
    return false;
  }

  const auto kind = static_cast<rt::HalV2MemoryDomainKind>(domain.kind);
  const auto coherency =
      static_cast<rt::HalV2MemoryCoherency>(domain.coherency);
  const auto host_access = domain.access & (RTFW_DEVICE_BUFFER_HOST_READ |
                                            RTFW_DEVICE_BUFFER_HOST_WRITE);
  const auto device_access = domain.access & (RTFW_DEVICE_BUFFER_DEVICE_READ |
                                              RTFW_DEVICE_BUFFER_DEVICE_WRITE);
  if ((kind == rt::HalV2MemoryDomainKind::host ||
       kind == rt::HalV2MemoryDomainKind::pinned_host) &&
      (host_access == 0 || (domain.ownership_modes &
                            rt::hal_v2_memory_ownership_borrowed_host) == 0)) {
    return false;
  }
  if (kind != rt::HalV2MemoryDomainKind::host &&
      kind != rt::HalV2MemoryDomainKind::pinned_host &&
      (domain.ownership_modes & rt::hal_v2_memory_ownership_borrowed_host) !=
          0) {
    return false;
  }
  if ((kind == rt::HalV2MemoryDomainKind::cuda_device ||
       kind == rt::HalV2MemoryDomainKind::dma_mapped ||
       kind == rt::HalV2MemoryDomainKind::peer) &&
      device_access == 0) {
    return false;
  }
  if ((kind == rt::HalV2MemoryDomainKind::imported ||
       kind == rt::HalV2MemoryDomainKind::dma_mapped ||
       kind == rt::HalV2MemoryDomainKind::peer) &&
      (domain.ownership_modes & rt::hal_v2_memory_ownership_borrowed_opaque) ==
          0) {
    return false;
  }
  if (coherency == rt::HalV2MemoryCoherency::host_coherent &&
      (host_access == 0 ||
       domain.required_synchronization != rt::hal_v2_memory_sync_none)) {
    return false;
  }
  if (coherency == rt::HalV2MemoryCoherency::explicit_flush_invalidate &&
      (domain.required_synchronization &
       (rt::hal_v2_memory_sync_flush | rt::hal_v2_memory_sync_invalidate)) ==
          0) {
    return false;
  }
  if (coherency == rt::HalV2MemoryCoherency::staged_copy &&
      (domain.required_synchronization &
       (rt::hal_v2_memory_sync_copy_to_device |
        rt::hal_v2_memory_sync_copy_from_device)) == 0) {
    return false;
  }
  if (coherency == rt::HalV2MemoryCoherency::device_only &&
      (host_access != 0 || device_access == 0)) {
    return false;
  }
  return true;
}

} // namespace

namespace rt::detail {

bool valid_memory_access(std::uint32_t access) noexcept {
  constexpr auto allowed =
      RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
      RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE;
  return access != 0 && (access & ~allowed) == 0;
}

bool valid_memory_coherency(std::uint32_t coherency) noexcept {
  return coherency >=
             static_cast<std::uint32_t>(HalV2MemoryCoherency::host_coherent) &&
         coherency <=
             static_cast<std::uint32_t>(HalV2MemoryCoherency::device_only);
}

bool valid_memory_synchronization(std::uint32_t synchronization) noexcept {
  constexpr auto allowed =
      hal_v2_memory_sync_flush | hal_v2_memory_sync_invalidate |
      hal_v2_memory_sync_copy_to_device | hal_v2_memory_sync_copy_from_device |
      hal_v2_memory_sync_timeline;
  return (synchronization & ~allowed) == 0;
}

std::uint32_t ownership_bit(HalV2MemoryOwnership ownership) noexcept {
  switch (ownership) {
  case HalV2MemoryOwnership::borrowed_host:
    return hal_v2_memory_ownership_borrowed_host;
  case HalV2MemoryOwnership::borrowed_opaque:
    return hal_v2_memory_ownership_borrowed_opaque;
  case HalV2MemoryOwnership::backend:
    return hal_v2_memory_ownership_backend;
  }
  return 0;
}

bool validate_opaque_handle(const HalV2OpaqueHandle &handle,
                            bool require_nonempty) noexcept {
  if (handle.struct_size < sizeof(handle) ||
      handle.size > hal_v2_opaque_handle_capacity ||
      (require_nonempty && handle.size == 0) || !all_zero(handle.reserved)) {
    return false;
  }
  return std::all_of(handle.bytes.begin() +
                         static_cast<std::ptrdiff_t>(handle.size),
                     handle.bytes.end(),
                     [](std::byte value) { return value == std::byte{0}; });
}

bool validate_memory_token(const HalV2MemoryToken &token,
                           bool require_submission_token) noexcept {
  return token.struct_size >= sizeof(token) &&
         token.extension_version == hal_v2_memory_topology_extension_version &&
         (!require_submission_token || token.submission_token != 0) &&
         validate_opaque_handle(token.native_token, false) &&
         all_zero(token.reserved);
}

bool validate_memory_topology_extension(
    const HalV2MemoryTopologyExtension &extension) noexcept {
  return extension.struct_size >= sizeof(extension) &&
         extension.extension_version ==
             hal_v2_memory_topology_extension_version &&
         extension.instance && extension.discover &&
         extension.register_memory && extension.unregister_memory &&
         extension.query_timestamp_correlation && all_zero(extension.reserved);
}

bool validate_memory_topology_snapshot(
    const HalV2MemoryTopologySnapshot &snapshot) noexcept {
  if (snapshot.struct_size < sizeof(snapshot) ||
      snapshot.extension_version != hal_v2_memory_topology_extension_version ||
      snapshot.memory_domain_count == 0 ||
      snapshot.memory_domain_count > hal_v2_memory_domain_capacity ||
      snapshot.topology_node_count == 0 ||
      snapshot.topology_node_count > hal_v2_topology_node_capacity ||
      snapshot.topology_link_count > hal_v2_topology_link_capacity ||
      snapshot.timestamp_domain_count == 0 ||
      snapshot.timestamp_domain_count > hal_v2_timestamp_domain_capacity ||
      !all_zero(snapshot.reserved) ||
      !unique_identity(snapshot.memory_domains, snapshot.memory_domain_count) ||
      !unique_identity(snapshot.topology_nodes, snapshot.topology_node_count) ||
      !unique_identity(snapshot.topology_links, snapshot.topology_link_count) ||
      !unique_identity(snapshot.timestamp_domains,
                       snapshot.timestamp_domain_count)) {
    return false;
  }

  for (std::size_t index = 0; index < snapshot.memory_domain_count; ++index) {
    const auto &domain = snapshot.memory_domains[index];
    if (!domain_consistent(domain) ||
        !contains_identity(snapshot.topology_nodes,
                           snapshot.topology_node_count,
                           domain.topology_node_identity) ||
        !contains_identity(snapshot.timestamp_domains,
                           snapshot.timestamp_domain_count,
                           domain.timestamp_domain_identity)) {
      return false;
    }
  }
  for (std::size_t index = 0; index < snapshot.topology_node_count; ++index) {
    const auto &node = snapshot.topology_nodes[index];
    if (node.struct_size < sizeof(node) ||
        node.extension_version != hal_v2_memory_topology_extension_version ||
        !valid_node_kind(node.kind) || node.reserved0 != 0 ||
        !all_zero(node.reserved)) {
      return false;
    }
  }
  bool has_peer_link = false;
  for (std::size_t index = 0; index < snapshot.topology_link_count; ++index) {
    const auto &link = snapshot.topology_links[index];
    if (link.struct_size < sizeof(link) ||
        link.extension_version != hal_v2_memory_topology_extension_version ||
        !valid_link_kind(link.kind) || link.reserved0 != 0 ||
        !all_zero(link.reserved) ||
        !contains_identity(snapshot.topology_nodes,
                           snapshot.topology_node_count,
                           link.source_node_identity) ||
        !contains_identity(snapshot.topology_nodes,
                           snapshot.topology_node_count,
                           link.destination_node_identity) ||
        (link.source_node_identity == link.destination_node_identity &&
         link.kind !=
             static_cast<std::uint32_t>(HalV2TopologyLinkKind::local))) {
      return false;
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      const auto &other = snapshot.topology_links[prior];
      if (other.source_node_identity == link.source_node_identity &&
          other.destination_node_identity == link.destination_node_identity &&
          other.kind == link.kind) {
        return false;
      }
    }
    has_peer_link =
        has_peer_link ||
        link.kind == static_cast<std::uint32_t>(HalV2TopologyLinkKind::peer);
    if (link.kind == static_cast<std::uint32_t>(HalV2TopologyLinkKind::peer)) {
      const auto node_kind = [&](std::uint64_t identity) {
        const auto begin = snapshot.topology_nodes.begin();
        const auto end =
            begin + static_cast<std::ptrdiff_t>(snapshot.topology_node_count);
        const auto found =
            std::find_if(begin, end, [identity](const auto &node) {
              return node.identity == identity;
            });
        return found == end ? 0u : found->kind;
      };
      if (node_kind(link.source_node_identity) !=
              static_cast<std::uint32_t>(
                  HalV2TopologyNodeKind::peer_endpoint) ||
          node_kind(link.destination_node_identity) !=
              static_cast<std::uint32_t>(
                  HalV2TopologyNodeKind::peer_endpoint)) {
        return false;
      }
    }
  }
  const bool has_peer_domain =
      std::any_of(snapshot.memory_domains.begin(),
                  snapshot.memory_domains.begin() +
                      static_cast<std::ptrdiff_t>(snapshot.memory_domain_count),
                  [](const auto &domain) {
                    return domain.kind == static_cast<std::uint32_t>(
                                              HalV2MemoryDomainKind::peer);
                  });
  if (has_peer_domain != has_peer_link) {
    return false;
  }
  for (std::size_t domain_index = 0;
       domain_index < snapshot.memory_domain_count; ++domain_index) {
    const auto &domain = snapshot.memory_domains[domain_index];
    if (domain.kind !=
        static_cast<std::uint32_t>(HalV2MemoryDomainKind::peer)) {
      continue;
    }
    const auto attached = std::any_of(
        snapshot.topology_links.begin(),
        snapshot.topology_links.begin() +
            static_cast<std::ptrdiff_t>(snapshot.topology_link_count),
        [&](const auto &link) {
          return link.kind ==
                     static_cast<std::uint32_t>(HalV2TopologyLinkKind::peer) &&
                 (link.source_node_identity == domain.topology_node_identity ||
                  link.destination_node_identity ==
                      domain.topology_node_identity);
        });
    if (!attached) {
      return false;
    }
  }

  for (std::size_t index = 0; index < snapshot.timestamp_domain_count;
       ++index) {
    const auto &domain = snapshot.timestamp_domains[index];
    if (domain.struct_size < sizeof(domain) ||
        domain.extension_version != hal_v2_memory_topology_extension_version ||
        !valid_timestamp_kind(domain.kind) || domain.reserved0 != 0 ||
        domain.tick_numerator_ns == 0 || domain.tick_denominator == 0 ||
        domain.monotonic > 1 || domain.resets_on_backend_reset > 1 ||
        domain.supports_correlation > 1 || domain.reserved1 != 0 ||
        domain.reserved2 != 0 || !all_zero(domain.reserved) ||
        (domain.wrap_ticks != 0 &&
         domain.tick_numerator_ns >
             std::numeric_limits<std::uint64_t>::max() / domain.wrap_ticks) ||
        (domain.supports_correlation == 0 &&
         domain.correlation_destination_identity != 0) ||
        (domain.supports_correlation != 0 &&
         (domain.correlation_destination_identity == domain.identity ||
          !contains_identity(snapshot.timestamp_domains,
                             snapshot.timestamp_domain_count,
                             domain.correlation_destination_identity)))) {
      return false;
    }
  }
  return contains_identity(snapshot.timestamp_domains,
                           snapshot.timestamp_domain_count,
                           snapshot.completion_timestamp_domain_identity);
}

Status discover_memory_topology(const HalV2MemoryTopologyExtension &extension,
                                HeterogeneousMemoryState &output) noexcept {
  output = {};
  if (!validate_memory_topology_extension(extension)) {
    return Status::invalid_argument;
  }
  HalV2MemoryTopologySnapshot candidate;
  candidate.memory_domain_count =
      static_cast<std::uint32_t>(hal_v2_memory_domain_capacity + 1u);
  candidate.topology_node_count =
      static_cast<std::uint32_t>(hal_v2_topology_node_capacity + 1u);
  candidate.topology_link_count =
      static_cast<std::uint32_t>(hal_v2_topology_link_capacity + 1u);
  candidate.timestamp_domain_count =
      static_cast<std::uint32_t>(hal_v2_timestamp_domain_capacity + 1u);
  HalV2Status status = HalV2Status::internal_error;
  try {
    status = extension.discover(extension.instance, &candidate);
  } catch (...) {
    return Status::internal_error;
  }
  const auto runtime_status = hal_v2_status_to_runtime(status);
  if (runtime_status != Status::ok) {
    return runtime_status;
  }
  if (!validate_memory_topology_snapshot(candidate)) {
    return Status::invalid_argument;
  }
  output.native_extension = true;
  output.extension = extension;
  output.snapshot = candidate;
  return Status::ok;
}

void make_implicit_host_memory_state(
    const HalV2Capabilities &capabilities,
    HeterogeneousMemoryState &output) noexcept {
  output = {};
  output.snapshot.memory_domain_count = 1;
  auto &domain = output.snapshot.memory_domains[0];
  domain.identity = 1;
  domain.kind = static_cast<std::uint32_t>(HalV2MemoryDomainKind::host);
  domain.ownership_modes = hal_v2_memory_ownership_borrowed_host;
  domain.maximum_bytes = capabilities.max_buffer_bytes;
  domain.byte_granularity = 1;
  domain.alignment = 1;
  domain.offset_granularity = 1;
  domain.access = RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
                  RTFW_DEVICE_BUFFER_DEVICE_READ |
                  RTFW_DEVICE_BUFFER_DEVICE_WRITE;
  domain.coherency =
      static_cast<std::uint32_t>(HalV2MemoryCoherency::host_coherent);
}

const HalV2MemoryDomain *
find_memory_domain(const HeterogeneousMemoryState &state,
                   std::uint64_t identity) noexcept {
  const auto begin = state.snapshot.memory_domains.begin();
  const auto end =
      begin + static_cast<std::ptrdiff_t>(state.snapshot.memory_domain_count);
  const auto found = std::find_if(begin, end, [identity](const auto &value) {
    return value.identity == identity;
  });
  return found == end ? nullptr : &*found;
}

const HalV2TimestampDomain *
find_timestamp_domain(const HeterogeneousMemoryState &state,
                      std::uint64_t identity) noexcept {
  const auto begin = state.snapshot.timestamp_domains.begin();
  const auto end = begin + static_cast<std::ptrdiff_t>(
                               state.snapshot.timestamp_domain_count);
  const auto found = std::find_if(begin, end, [identity](const auto &value) {
    return value.identity == identity;
  });
  return found == end ? nullptr : &*found;
}

const HalV2MemoryDomain *
find_legacy_host_domain(const HeterogeneousMemoryState &state,
                        std::uint32_t access) noexcept {
  const auto begin = state.snapshot.memory_domains.begin();
  const auto end =
      begin + static_cast<std::ptrdiff_t>(state.snapshot.memory_domain_count);
  const auto found = std::find_if(begin, end, [access](const auto &domain) {
    return domain.kind ==
               static_cast<std::uint32_t>(HalV2MemoryDomainKind::host) &&
           (domain.ownership_modes & hal_v2_memory_ownership_borrowed_host) !=
               0 &&
           (access & ~domain.access) == 0 &&
           domain.coherency == static_cast<std::uint32_t>(
                                   HalV2MemoryCoherency::host_coherent) &&
           domain.required_synchronization == hal_v2_memory_sync_none;
  });
  return found == end ? nullptr : &*found;
}

bool validate_timestamp_correlation(
    const HalV2TimestampCorrelationQuery &query,
    const HalV2TimestampCorrelation &correlation) noexcept {
  return correlation.struct_size >= sizeof(correlation) &&
         correlation.extension_version ==
             hal_v2_memory_topology_extension_version &&
         correlation.source_domain_identity == query.source_domain_identity &&
         correlation.destination_domain_identity ==
             query.destination_domain_identity &&
         correlation.generation != 0 &&
         correlation.uncertainty_ns !=
             std::numeric_limits<std::uint64_t>::max() &&
         correlation.source_value <= std::numeric_limits<std::uint64_t>::max() -
                                         correlation.uncertainty_ns &&
         correlation.destination_value <=
             std::numeric_limits<std::uint64_t>::max() -
                 correlation.uncertainty_ns &&
         all_zero(correlation.reserved);
}

} // namespace rt::detail

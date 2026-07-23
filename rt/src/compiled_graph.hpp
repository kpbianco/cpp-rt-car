#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <rt/graph.hpp>
#include <rt/runtime.hpp>

namespace rt::detail {

struct GraphDependency {
    PhaseHandle prerequisite;
    PhaseHandle dependent;
};

struct GraphResourceAccess {
    PhaseHandle phase;
    ResourceHandle resource;
    ResourceAccess access;
};

struct GraphCompileDiagnostic {
    Status status = Status::ok;
    PhaseHandle first_phase{};
    PhaseHandle second_phase{};
    ResourceHandle resource{};
};

// Compiles a deterministic topological order. The output is transactional: it
// is replaced only when the complete graph validates successfully.
[[nodiscard]] Status compile_graph(
    std::uint32_t graph_owner,
    std::size_t phase_count,
    std::size_t resource_count,
    std::span<const GraphDependency> dependencies,
    std::span<const GraphResourceAccess> accesses,
    std::vector<PhaseHandle>& output_order,
    GraphCompileDiagnostic& diagnostic) noexcept;

} // namespace rt::detail

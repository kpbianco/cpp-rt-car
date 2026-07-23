#include "compiled_graph.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <new>
#include <numeric>
#include <queue>
#include <utility>
#include <vector>

namespace {

using Adjacency = std::vector<std::vector<std::uint32_t>>;

void mark_reachable(
    std::uint32_t start,
    const Adjacency& adjacency,
    std::vector<std::uint64_t>& visited,
    std::vector<std::uint32_t>& stack,
    std::uint64_t visit_token) {
    stack.clear();
    stack.push_back(start);
    visited[start] = visit_token;

    while (!stack.empty()) {
        const auto phase = stack.back();
        stack.pop_back();
        for (const auto adjacent : adjacency[phase]) {
            if (visited[adjacent] == visit_token) {
                continue;
            }
            visited[adjacent] = visit_token;
            stack.push_back(adjacent);
        }
    }
}

} // namespace

namespace rt::detail {

Status compile_graph(
    std::uint32_t graph_owner,
    std::size_t phase_count,
    std::size_t resource_count,
    std::span<const GraphDependency> dependencies,
    std::span<const GraphResourceAccess> accesses,
    std::vector<PhaseHandle>& output_order,
    GraphCompileDiagnostic& diagnostic) noexcept {
    diagnostic = {};

    if (phase_count > graph_handle_index_mask ||
        resource_count > graph_handle_index_mask) {
        diagnostic.status = Status::capacity_exceeded;
        return diagnostic.status;
    }

    try {
        Adjacency successors(phase_count);
        Adjacency predecessors(phase_count);
        std::vector<std::size_t> indegree(phase_count, 0);

        for (const auto& dependency : dependencies) {
            if (!dependency.prerequisite.valid() ||
                !dependency.dependent.valid() ||
                dependency.prerequisite.owner() != graph_owner ||
                dependency.dependent.owner() != graph_owner ||
                dependency.prerequisite.index() >= phase_count ||
                dependency.dependent.index() >= phase_count) {
                diagnostic.status = Status::invalid_handle;
                diagnostic.first_phase = dependency.prerequisite;
                diagnostic.second_phase = dependency.dependent;
                return diagnostic.status;
            }
            successors[dependency.prerequisite.index()].push_back(
                dependency.dependent.index());
            predecessors[dependency.dependent.index()].push_back(
                dependency.prerequisite.index());
            ++indegree[dependency.dependent.index()];
        }

        std::priority_queue<
            std::uint32_t,
            std::vector<std::uint32_t>,
            std::greater<>> ready;
        for (std::uint32_t phase = 0; phase < phase_count; ++phase) {
            if (indegree[phase] == 0) {
                ready.push(phase);
            }
        }

        std::vector<PhaseHandle> candidate_order;
        candidate_order.reserve(phase_count);
        while (!ready.empty()) {
            const auto phase = ready.top();
            ready.pop();
            candidate_order.push_back(PhaseHandle{graph_owner, phase});
            for (const auto successor : successors[phase]) {
                if (--indegree[successor] == 0) {
                    ready.push(successor);
                }
            }
        }

        if (candidate_order.size() != phase_count) {
            diagnostic.status = Status::graph_cycle;
            std::vector<std::uint8_t> color(phase_count, 0);
            std::vector<std::size_t> next_successor(phase_count, 0);
            std::vector<std::uint32_t> depth_first_stack;
            depth_first_stack.reserve(phase_count);

            for (std::uint32_t root = 0; root < phase_count; ++root) {
                if (color[root] != 0) {
                    continue;
                }
                color[root] = 1;
                depth_first_stack.push_back(root);
                while (!depth_first_stack.empty()) {
                    const auto phase = depth_first_stack.back();
                    if (next_successor[phase] ==
                        successors[phase].size()) {
                        color[phase] = 2;
                        depth_first_stack.pop_back();
                        continue;
                    }

                    const auto successor =
                        successors[phase][next_successor[phase]++];
                    if (color[successor] == 1) {
                        diagnostic.first_phase =
                            PhaseHandle{graph_owner, successor};
                        return diagnostic.status;
                    }
                    if (color[successor] == 0) {
                        color[successor] = 1;
                        depth_first_stack.push_back(successor);
                    }
                }
            }
            diagnostic.status = Status::internal_error;
            return diagnostic.status;
        }

        std::vector<std::size_t> access_order(accesses.size());
        std::iota(access_order.begin(), access_order.end(), std::size_t{0});
        for (const auto& access : accesses) {
            if (!access.phase.valid() || !access.resource.valid() ||
                access.phase.owner() != graph_owner ||
                access.resource.owner() != graph_owner ||
                access.phase.index() >= phase_count ||
                access.resource.index() >= resource_count ||
                (access.access != ResourceAccess::read &&
                 access.access != ResourceAccess::write)) {
                diagnostic.status = Status::invalid_handle;
                diagnostic.first_phase = access.phase;
                diagnostic.resource = access.resource;
                return diagnostic.status;
            }
        }
        std::sort(
            access_order.begin(),
            access_order.end(),
            [&](std::size_t left, std::size_t right) {
                const auto& lhs = accesses[left];
                const auto& rhs = accesses[right];
                if (lhs.resource.index() != rhs.resource.index()) {
                    return lhs.resource.index() < rhs.resource.index();
                }
                return lhs.phase.index() < rhs.phase.index();
            });

        std::vector<std::uint64_t> reachable_after(phase_count, 0);
        std::vector<std::uint64_t> reachable_before(phase_count, 0);
        std::vector<std::uint32_t> stack;
        stack.reserve(phase_count);
        std::uint64_t visit_token = 0;

        std::size_t resource_begin = 0;
        while (resource_begin < access_order.size()) {
            const auto resource =
                accesses[access_order[resource_begin]].resource;
            std::size_t resource_end = resource_begin + 1;
            while (resource_end < access_order.size() &&
                   accesses[access_order[resource_end]].resource == resource) {
                ++resource_end;
            }

            for (std::size_t writer_index = resource_begin;
                 writer_index < resource_end;
                 ++writer_index) {
                const auto& writer = accesses[access_order[writer_index]];
                if (writer.access != ResourceAccess::write) {
                    continue;
                }

                if (visit_token ==
                    std::numeric_limits<std::uint64_t>::max()) {
                    std::fill(
                        reachable_after.begin(),
                        reachable_after.end(),
                        std::uint64_t{0});
                    std::fill(
                        reachable_before.begin(),
                        reachable_before.end(),
                        std::uint64_t{0});
                    visit_token = 1;
                } else {
                    ++visit_token;
                }
                mark_reachable(
                    writer.phase.index(),
                    successors,
                    reachable_after,
                    stack,
                    visit_token);
                mark_reachable(
                    writer.phase.index(),
                    predecessors,
                    reachable_before,
                    stack,
                    visit_token);

                for (std::size_t other_index = resource_begin;
                     other_index < resource_end;
                     ++other_index) {
                    const auto& other = accesses[access_order[other_index]];
                    if (writer.phase == other.phase) {
                        continue;
                    }
                    if (reachable_after[other.phase.index()] != visit_token &&
                        reachable_before[other.phase.index()] != visit_token) {
                        diagnostic.status = Status::resource_conflict;
                        diagnostic.first_phase = writer.phase;
                        diagnostic.second_phase = other.phase;
                        diagnostic.resource = resource;
                        return diagnostic.status;
                    }
                }
            }
            resource_begin = resource_end;
        }

        output_order.swap(candidate_order);
        diagnostic.status = Status::ok;
        return Status::ok;
    } catch (const std::bad_alloc&) {
        diagnostic.status = Status::resource_exhausted;
        return diagnostic.status;
    } catch (...) {
        diagnostic.status = Status::internal_error;
        return diagnostic.status;
    }
}

} // namespace rt::detail

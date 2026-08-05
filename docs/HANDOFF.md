# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. M14, M14.1, and M15 are complete
at baseline `8dabeb3747a20717ce02c99a3d3c0d9deb1a3532`. M16 is active, and this
worktree implements approved batch M16-02: deterministic cross-rate data
semantics over the M16-01 reference timeline. `contracts/active-batch.yaml` is
binding.

## Read first

1. `AGENTS.md`
2. `contracts/repo-profile.yaml`
3. `contracts/profile-requirements.yaml`
4. `contracts/active-batch.yaml`
5. `.agents/skills/rtfw-assurance/SKILL.md`
6. `docs/CURRENT_STATE.md`
7. `docs/product_contract.md`
8. `docs/architecture.md`
9. `docs/roadmap.md`
10. `docs/compiled_graph.md`
11. `docs/host_runtime.md`
12. `docs/executor.md`
13. `docs/time_platform.md`
14. `docs/memory_plan.md`
15. `docs/determinism_replay.md`

Canonical control plane:
`/home/kbianco/.local/share/portfolio-control/worktrees/control/cpp-rt-car/products/cpp-rt-car`
at revision `e00e41f067672bfe98209746ce4fc30780fdd160`.

## Implemented boundary

The installed C++ API adds instance-owned rate-domain handles, copied bounded
registrations, phase bindings, compiled-domain/binding records, and fixed-copy
reference-release inspectors in the existing SDK headers. Finalization
transactionally compiles the exact `[0, lcm)` epoch-zero plan with a 64-domain,
64-substep-per-domain, and 65,536-release ceiling. Period ratio metadata is the
reduced exact ratio against registration-order domain zero.

Every enabled CPU or device phase has one owner. Duplicate, rebound, missing,
foreign/stale, malformed, empty-domain, overflow, and capacity cases fail while
the runtime remains configuring and correctable. Equal-time ordering is domain
registration, compiled phase, and substep. Budget/WCET is untrusted planning
metadata and is not admission or timing evidence.

The plan changes graph/replay identity only when explicit rate metadata exists.
Checkpoint/input-log schemas remain version 1. Dynamic rate storage is acquired
only during configuration/finalization, accounted once within runtime control,
and never mutated by inspection. Host and periodic dispatch remain complete-
graph, once-per-frame behavior.

M16-02 copies fixed-payload sample-and-hold channels between CPU phases in
different explicit domains. It compiles two immutable selections per consumer
release: first-supercycle selection, including the copied initial sample, and
repeating-supercycle selection, including exact prior-cycle provenance. Each
record exposes endpoint/reference/substep identities, signed cycle offset,
integer age, held state, provenance, and inclusive freshness classification.
One two-slot exact-generation SPSC store is preallocated per channel. Store
operations are bounded single attempts and are not connected to execution.

## Protected decisions

- C ABI v8, 70 exports/fingerprint, SONAME 8, and device ABI v1 are unchanged.
- Runtime schema 7/25 keys, observability and artifact schemas, installed
  headers/targets, aliases, support matrices, 1.2.1, and Apache-2.0 are unchanged.
- The M15 six-row MemoryPlan and provider boundary remain unchanged.
- Active cross-rate transfer, admission, late/catch-up, shedding/recovery, and
  telemetry evolution remain outside M16-02.
- No compiled plan, mock, hosted CI, or local run is hardware, latency, RT1,
  RT2, signing, release, deployment, or production evidence.

## Completion output

Run every local command in the active batch, ending with
`./scripts/agent-verify.sh full`. Retain exact results and acceptance mapping in
`docs/evidence/M16-02-2026-08-05.md`. Mandatory GitHub CI and human public-API,
selection, store lifetime/memory-ordering, identity, accounting, determinism, lifecycle, and
compatibility review remain external gates. M16 and CAP-M16 remain incomplete.

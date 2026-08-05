# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. M14, M14.1, and M15 are complete
at baseline `9c350ab5b66e37e90aef7e96939c71df04101b27`. M16 is active, and this
worktree implements approved batch M16-04: optional CPU shedding/recovery and
separate versioned rate-action telemetry over the M16-01 through M16-03 plans.
`contracts/active-batch.yaml` is binding.

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
at revision `92abc5d737b95133a4dd9e68170f29cf55d26371`.

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
graph, once-per-frame behavior when the additive execution policy is absent.

M16-02 copies fixed-payload sample-and-hold channels between CPU phases in
different explicit domains. It compiles two immutable selections per consumer
release: first-supercycle selection, including the copied initial sample, and
repeating-supercycle selection, including exact prior-cycle provenance. Each
record exposes endpoint/reference/substep identities, signed cycle offset,
integer age, held state, provenance, and inclusive freshness classification.
One two-slot exact-generation SPSC store is preallocated per channel. M16-03
connects those stores only for explicitly active CPU plans. Active finalization
performs conservative serialized mandatory-only admission and rejects D1,
device domains, invalid budgets/deadlines, cross-domain ordinary dependencies, and
skip producers. Positive contiguous logical/nominal windows dispatch the exact
reference order through the existing executor, with bounded skip, catch-up,
hold, degrade, or fail decisions and exact functional summaries.

Active producers stage one exact-size payload for each outgoing channel before
commit; consumers copy only the exact initial, produced, or held generation.
The active cursor, epoch, action/fault state, aliases, generations, and bytes
are one reserved generic checkpoint record. Schema numbers stay unchanged, and
active schema-1 input logs/replay are rejected.

M16-04 accepts bounded optional CPU domains that are not cross-rate endpoints.
Mandatory releases alone drive positive late/on-time hysteresis thresholds.
Shedding order is lower criticality then later registration; recovery reverses
that order, and each transition affects the next release immediately. Policy
state is retained transactionally in the existing active generic record.

A separate fixed 160-byte rate-action schema-1 record, fixed-capacity atomic
ring, runtime-bound cursors, and 20 counters/gauges report execution, omission,
transitions, loss, and policy provenance. Zero capacity explicitly drops every
publication. History/counters are process-local, are not checkpointed, and do
not alter global observability schema 2 or dispatch decisions.

## Protected decisions

- C ABI v8, 70 exports/fingerprint, SONAME 8, and device ABI v1 are unchanged.
- Runtime schema 7/25 keys, observability and artifact schemas, installed
  headers/targets, aliases, support matrices, 1.2.1, and Apache-2.0 are unchanged.
- The M15 six-row MemoryPlan and provider boundary remain unchanged.
- Active D1, action replay, and device-rate execution remain unsupported.
- No compiled plan, mock, hosted CI, or local run is hardware, latency, RT1,
  RT2, signing, release, deployment, or production evidence.

## Completion output

Run every local command in the active batch, ending with
`./scripts/agent-verify.sh full`. Retain exact results and acceptance mapping in
`docs/evidence/M16-04-2026-08-05.md`. Mandatory GitHub CI and human public-API,
policy, telemetry memory-ordering, checkpoint, identity, accounting,
determinism, lifecycle, and compatibility review remain external gates. M16
and CAP-M16 remain incomplete until those gates pass.

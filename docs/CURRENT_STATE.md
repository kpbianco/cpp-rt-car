# Current state

Last audited: 2026-08-05
Batch baseline: `9c350ab5b66e37e90aef7e96939c71df04101b27`

## Product state

- Release: 1.2.1 portable RT0.
- Stable boundary: C ABI v8 with 70 exports and SONAME 8; device ABI v1.
- Runtime/profile boundary: schema 7 with 25 keys; observability schema 2;
  checkpoint and input-log schema 1.
- License: Apache-2.0.
- M14, M14.1, and M15 are complete at the audited baseline.
- M16 is active and remains incomplete. The current approved implementation
  contract is `contracts/active-batch.yaml` (`M16-04`).

## M16-01 and M16-02 implementation

The worktree adds an additive C++ rate-domain model without changing the
installed header or target inventory. A runtime can copy up to 64 stable
domains, bind each CPU or device phase to exactly one instance-owned domain,
and finalize an immutable epoch-zero reference interval. Periods and releases
use integral nanoseconds. Domain names, periods, substeps, deadlines,
non-binding budget/WCET metadata, criticality, optionality, and phase ownership
are validated and frozen.

The reference interval is `[0, lcm(periods))`. Checked integer gcd/lcm,
multiplication, addition, and count arithmetic reject overflow or more than
65,536 release entries before any provider, device, native-policy, thread, or
callback action. Equal-time releases are ordered by domain registration,
compiled phase order, then substep ordinal. Inspectors copy fixed records and
do not allocate or mutate the plan after start.

Explicit rate semantics participate in graph/replay compatibility identity.
A runtime with no explicit model follows the pre-M16 identity path exactly.
Rate storage is counted once inside `MemoryPlan::runtime_control_bytes` and the
existing M15 runtime-control extent; the six-row plan equation and three-region
provider boundary are unchanged.

M16-02 adds copied, bounded C++ cross-rate channels between CPU phases in
different explicit rate domains. Finalization emits immutable first-cycle and
repeating-cycle sample-and-hold selections for every consumer release. The
existing total reference order decides same-time visibility; initial samples,
real prior-cycle sources (`source_cycle_offset == -1`), held generations, and
fresh/stale metadata remain distinct. Maximum age is inclusive, with zero
accepting only age zero and `UINT64_MAX` explicitly unbounded.

Each compiled channel owns a two-slot, exact-generation SPSC snapshot store.
Publication copies into a non-visible slot before a release publication; copy
claims a committed generation before reading. Calls are single-attempt and
return typed capacity, not-ready, stale, or malformed results without waiting,
allocation, searching, or substituting another generation. Runtime dispatch
does not invoke these stores yet.

## M16-03 implementation

M16-03 adds an opt-in configuring-only C++ `RateExecutionPolicy`. Without that
policy, the M16-01/M16-02 model retains exact reference-only identity and
complete-graph once-per-frame dispatch. With it, finalization accepts only D0,
mandatory CPU domains with positive feasible budgets/deadlines and same-domain
ordinary dependencies. A conservative serialized integer admission pass checks
every reference record and the idle supercycle boundary before runtime-side
provider, thread, device, callback, or checkpoint effects.

Active steps require positive half-open `[cursor, cursor + delta)` windows and
contiguous nominal timestamps. Due reference records repeat by checked
supercycle arithmetic and execute serially through the existing executor.
Per-domain skip, bounded catch-up, hold, degrade, and fail actions are explicit;
aggregate counting handles large omitted prefixes, and the positive global
record cap rejects overflow without backlog or spill execution. Periodic mode
passes its absolute release as the nominal timestamp.

Active callbacks receive a nullable rate-release view. Producers stage one
exact-size payload per outgoing channel, and successful releases publish
complete generations through the existing two-slot stores. Consumers copy the
exact current initial, produced, or held generation with age/freshness results.
The cursor, nominal epoch, degradation/fault state, generation aliases, and
committed channel bytes are retained in one internal canonical state record
through the unchanged checkpoint schema-1 generic record mechanism. Active
schema-1 input-log export and replay are rejected explicitly.

## M16-04 implementation

M16-04 appends a copied policy version, positive late/recovery hysteresis
thresholds, and a bounded telemetry capacity. Active finalization now accepts
optional CPU D0 domains while admission remains mandatory-only. Optional work
starts active, sheds by increasing criticality and reverse registration order,
and recovers in the exact reverse order. Only settled mandatory releases drive
the streaks; transitions affect the next release in total order, including at
the same timestamp. Optional cross-rate endpoints remain rejected.

The runtime owns a separate fixed-capacity rate-action schema-1 ring and 20
counters/gauges. Publication is one bounded nonblocking attempt with explicit
overwrite, contention, and zero-capacity loss. Runtime-bound cursors report
exact gaps and inspection rejects an active step or periodic loop. This stream
does not change global observability schema 2 and is not checkpointed or an
action-replay log.

The canonical active generic checkpoint record conditionally retains policy
version, thresholds, streaks, deterministic optional order, and shed state.
Reference-only and mandatory-only records remain byte-identical. Finalization
accounts policy, checkpoint, ring, slot, and counter storage once inside the
existing rate-plan/runtime-control extent and six-row memory equation.

## Boundary and remaining work

M16-04 establishes portable RT0 functional optional-work shedding/recovery and
versioned rate-action observation. Action-aware replay, active D1, device-rate
execution, and qualification remain outside this batch. M16 and CAP-M16 remain
incomplete until mandatory CI and human review pass.

Local deterministic verification can establish exact integer compilation,
functional lifecycle, package compatibility, bounded storage, replay identity,
and allocation-free inspection/frames on the named host. Mandatory GitHub CI
and human API/arithmetic/identity/accounting/determinism/lifecycle review remain
merge gates. No physical hardware, HIL, field, latency, RT1, RT2, Unreal,
signing, release, staging, deployment, or production validation is claimed.

## Next action

Complete the M16-04 local verification contract, retain
`docs/evidence/M16-04-2026-08-05.md`, and submit the bounded diff for mandatory
CI and human review. Do not mark M16 or CAP-M16 complete before those gates.

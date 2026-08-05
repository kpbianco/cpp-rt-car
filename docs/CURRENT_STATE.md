# Current state

Last audited: 2026-08-05
Batch baseline: `9dfcee7bc8092de768292ba35ad9bd74e5a5992e`

## Product state

- Release: 1.2.1 portable RT0.
- Stable boundary: C ABI v8 with 70 exports and SONAME 8; device ABI v1.
- Runtime/profile boundary: schema 7 with 25 keys; observability schema 2;
  checkpoint and input-log schema 1.
- License: Apache-2.0.
- M14, M14.1, and M15 are complete at the audited baseline.
- M16 is active and remains incomplete. The current approved implementation
  contract is `contracts/active-batch.yaml` (`M16-01`).

## M16-01 implementation

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

## Boundary and remaining work

M16-01 is model-and-reference-plan only. `step()` and `run_periodic()` still
execute the complete compiled graph once per host frame. The reference plan
does not dispatch domains and does not implement cross-rate sample/hold or
freshness, admission feasibility, late skip/catch-up/hold/degrade/fail policy,
optional-work shedding/recovery, or new trace/metric fields. Those remain M16
work, so M16 and CAP-M16 are incomplete.

Local deterministic verification can establish exact integer compilation,
functional lifecycle, package compatibility, bounded storage, replay identity,
and allocation-free inspection/frames on the named host. Mandatory GitHub CI
and human API/arithmetic/identity/accounting/determinism/lifecycle review remain
merge gates. No physical hardware, HIL, field, latency, RT1, RT2, Unreal,
signing, release, staging, deployment, or production validation is claimed.

## Next action

Complete the M16-01 local verification contract, retain
`docs/evidence/M16-01-2026-08-05.md`, and submit the bounded diff for mandatory
CI and human review. Do not mark M16 or CAP-M16 complete from this batch.

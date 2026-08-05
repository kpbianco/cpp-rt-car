# Rate-action telemetry schema 1

M16-04 adds a C++-only rate-action stream beside the global observability
surface. Its schema version is 1; global observability remains schema 2 with
trace IDs 1-14 and metric IDs 0-31. Rate telemetry is bounded RT0 functional
evidence, not an authenticated replay log, measured WCET, deadline proof, or
hardware/RT qualification record.

## Policy and bounds

`RateExecutionPolicy` copies a positive dispatch limit, nonzero host policy
version, positive late and on-time thresholds, and a telemetry capacity no
greater than 65,536. Thresholds are also bounded at 65,536. Capacity zero is
valid: every range or singleton publication attempt reserves a sequence and is
reported as dropped. Plans with no optional domain preserve the M16-03 graph
and replay identity; telemetry capacity is observational. Optional plans hash
the policy version, thresholds, and compiled optional ordering.

All optional domains start active. Shed order is lower criticality first and,
within a criticality, later registration first. Recovery reverses that exact
order. Only a settled mandatory release updates hysteresis. Late resets the
on-time streak; on-time resets the late streak. A threshold changes exactly
one domain and resets its triggering streak. Terminal fail, callback failure,
dispatch capacity, and arithmetic failure cause no later transition. A state
change is effective for the next total-order release, including a later
registration at the same logical timestamp.

Late optional skip/hold prefixes aggregate independently up to the next
mandatory or executable total-order barrier. Their bounded range records do
not replay every omitted supercycle and do not advance either hysteresis
streak.

## Fixed numeric tables

| Action ID | Name |
| ---: | --- |
| 0 | `execute_on_time` |
| 1 | `execute_catch_up` |
| 2 | `skip` |
| 3 | `hold` |
| 4 | `execute_degraded` |
| 5 | `fail` |
| 6 | `optional_shed` |

| Transition ID | Name |
| ---: | --- |
| 0 | `none` |
| 1 | `shed` |
| 2 | `recover` |

| Reason ID | Name |
| ---: | --- |
| 0 | `on_time` |
| 1 | `deadline_late` |
| 2 | `already_shed` |
| 3 | `late_threshold` |
| 4 | `on_time_threshold` |
| 5 | `callback_failure` |
| 6 | `dispatch_capacity` |
| 7 | `arithmetic_failure` |

Counter IDs 0-19 are, in order: due domain releases, executed reference
records, late domain releases, caught-up releases, skipped releases, held
releases, degraded releases, failed releases, optional due releases, optional
executed releases, shed releases, shed transitions, recovery transitions,
records emitted, records overwritten, records dropped, currently shed domains,
policy version, rejected reference records, and stale reads. IDs 16 and 17 are
gauges; every other ID is a cumulative counter. `stale_reads` retains its
cross-rate freshness meaning and does not count telemetry gaps.

## Records, ranges, and loss

`RateActionRecord` is a fixed 160-byte record. It carries schema and record
size, monotonic record sequence, host policy version, runtime and frame
identity, domain registration index, first domain-release sequence, first
logical and nominal release, release-period stride, exact release and reference
record counts, optional and late flags, action/reason/transition, status,
degradation, transitioned-domain index, and the exact before/after 64-bit shed
masks. Expanding a range adds the stride to logical and nominal time and one to
release sequence for each release. A range never crosses a domain, action,
reason, status, late state, degradation state, transition, or shed-mask change;
a transition record is a singleton.

The runtime allocates the ring and counters during finalization. A producer
reserves one sequence and tries one slot without waiting. A busy slot is a
drop; replacing a committed slot is an overwrite. Slow readers cannot change
dispatch. Metadata reports schema, record size, counter count, policy version,
runtime identity, capacity, next sequence, and emitted/overwritten/dropped
totals. Runtime-bound cursors report missing sequences as exact gaps. Malformed,
foreign, or future cursors fail without advancing. Inspection is a non-RT host
operation and rejects an active step, periodic loop, or replay.

## Checkpoint and memory boundary

For optional plans, the existing canonical `rtfw.rate-dispatch` generic state
record appends the policy version and thresholds, both streaks, exact shed
mask/count, and compiled order. Restore validates the complete tail before any
live state changes. Mandatory-only active record bytes retain the M16-03 form.
Checkpoint schema 1 and its generic codec do not change. Telemetry slots,
history, cumulative counters, and caller cursors are process-local and are not
checkpointed; only the live shedding policy state is canonical.

The shedding state, optional order, checkpoint tail, ring control/slots, and
counters are counted exactly once in `rate_plan_bytes` and the existing M15
runtime-control extent. The six-row `MemoryPlan` equation is unchanged. Active
execution, omission, publication, inspection, restore, and checked stop do not
perform ordinary heap allocation after successful start.

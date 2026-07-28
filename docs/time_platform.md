# Time, Watchdog, and Platform Contract

Release 0.6 introduced, and release 1.2 retains, the M5 RT0 time/platform
surface in `rt::Runtime`.
It adds finite self-paced execution, a one-shot frame watchdog, frame-thread
degradation state, and a strict reportable Linux platform preflight. These are
functional contracts, not a hard-real-time or RT2 qualification.

The legacy `SimCore::run()`, `rt::Watchdog`, hardening scripts, and limp-mode
controller are separate experiments and do not inherit this contract.

## Two explicit time modes

Host-driven `Runtime::step()` remains the default embedding operation. The host
supplies a frame index, simulation delta, and optional absolute deadline;
`step()` never paces or sleeps.

`Runtime::run_periodic()` is the separate runtime-paced operation. It executes
a finite number of frames on the calling frame thread. For frame offset `i`,
the release and deadline are:

```text
release(i)  = first_release + i * period
deadline(i) = release(i) + relative_deadline
```

If `first_release` is omitted, the runtime samples its clock once and uses that
value as the epoch. Every wait is an absolute `sleep_until_ns(release(i))`.
A late frame does not move the epoch and no configured frame is skipped:
subsequent late releases execute immediately and may run back-to-back. The
runtime rejects zero counts, nonpositive durations, and any release, deadline,
next-release, or frame-index overflow before the first wait.

A custom `RuntimeClock` must return monotonic nanoseconds in one domain,
implement absolute waiting, and report `supports_absolute_sleep()`. Host-driven
steps remain usable with a clock that cannot wait; periodic execution returns
`clock_failure`.

`PeriodicFrameResult` reports status, frame index, release, wake, start, finish,
signed deadline slack, miss state, watchdog state, and degradation level.
`PeriodicRunResult` reports executed frames, deadline misses, watchdog events,
the final degradation level, the epoch, next release, and last frame. An
optional observer runs synchronously on the frame thread after each attempted
step. Its execution time therefore affects following releases, and host
observer code is outside the runtime's allocation/blocking contract. An
observer failure stops the finite loop with `callback_failed`.

## Watchdog and degradation

`watchdog_timeout_ns == 0` disables the watchdog. A nonzero timeout creates one
runtime-owned service lane in `start()` and joins it in `stop()`. Each step
arms one deadline relative to that step's measured start.
Each arm produces at most one event. This remains true when both the service
clock and the runtime clock observe expiry.

The service lane can mark an overrun while a callback is still running, but it:

- never invokes host code;
- never mutates degradation state;
- cannot preempt, cancel, or terminate a stuck callback.

Once the graph quiesces, the calling frame thread consumes the one-shot result.
If the frame finished at or after the watchdog deadline, it records one event
and increments the runtime-owned degradation level up to
`watchdog_max_degradation_level`. Callbacks in the overrun frame observe the
previous level; callbacks in following frames observe the committed level
through `CallbackContext::degradation_level`.

The watchdog arm/disarm path uses atomics and a notification only. Its mutex
and condition-variable wait are confined to the non-RT service lane. The M4
allocation gate runs complete frames with the watchdog armed.

Degradation is a signal, not automatic workload shedding. The host callback
must select cheaper work when it sees a higher level. There is no recovery or
level-decrease policy in 1.2.

## Strict platform preflight

`platform_preflight_mode` defaults to `disabled`. Disabled mode performs no
probe and does not mutate the host.

In `strict` mode, `start()` runs preflight after finalization and before any
executor or watchdog thread is created. Success requires exactly one passing
result for every prerequisite:

| Check | Linux condition |
| --- | --- |
| Absolute monotonic clock | The runtime clock supports absolute waits and the standard steady clock is steady |
| Realtime kernel | `/sys/kernel/realtime` reports enabled, or `uname` identifies `PREEMPT_RT` |
| Memory-lock limit | `RLIMIT_MEMLOCK` covers the finalized runtime memory plan |
| Locked memory | `VmLck` in `/proc/self/status` covers the finalized runtime memory plan |
| Isolated CPU affinity | The calling thread's affinity mask is nonempty and entirely contained in the kernel isolated-CPU list |
| Realtime scheduler | The calling thread uses `SCHED_FIFO` or `SCHED_RR` with positive priority |

The native probe is read-only: it does not call `mlockall`, change affinity,
raise priority, edit limits, or alter system policy. On unsupported platforms,
Linux-specific checks report `unsupported`, so strict mode fails closed.

A failure returns `platform_preflight_failed`, leaves the runtime finalized,
and starts no runtime threads. The complete fixed-capacity report remains
available through `platform_preflight_report()` or
`rtfw_get_platform_preflight_report()`, including per-check status, system
error, and explanation. C++ tests can inject a `PlatformPreflightProbe` for
deterministic validation.

Passing this preflight does not establish RT2. It does not validate BIOS and
power settings, IRQ placement, driver/device behavior, PCIe topology, thermal
behavior, workload bounds, or measured deadline distributions. Those remain
deployment qualification requirements in the
[product contract](product_contract.md).

## C ABI

Stable ABI version 8 retains the M5 surface:

- watchdog and preflight fields in `rtfw_config`;
- `rtfw_run_periodic()` and its initialized config/result records;
- periodic observer results with release/wake/start/finish/slack;
- watchdog and degradation fields in callback and step results;
- initialized fixed-capacity preflight reports;
- `RTFW_STATUS_PLATFORM_PREFLIGHT_FAILED` and
  `RTFW_STATUS_CLOCK_FAILURE`.

All public structures retain size and reserved-field validation. The ABI remains
covered by the M11 compatibility and export policy.

## Evidence and boundaries

- implementation: `rt/src/host_runtime.cpp`,
  `rt/src/watchdog_monitor.cpp`,
  `rt/src/native_platform_preflight.cpp`;
- fake-clock, no-drift, deadline, watchdog, and degradation tests:
  `tests/test_periodic_runtime.cpp`;
- fail-closed preflight tests: `tests/test_platform_preflight.cpp`;
- armed-watchdog allocation gate: `tests/test_trace_noalloc.cpp`;
- dynamic C ABI coverage: `tests/test_cabi_dlopen.c`;
- finite C and C++ periodic samples:
  `samples/embed_c/mini_app.c`,
  `samples/embed_cpp/mini_app.cpp`.

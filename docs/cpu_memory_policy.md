# CPU and memory policy model

M15-01 adds the C++ policy and report model used by later M15 startup and
memory-provider batches. M15-02 applies the supported thread-only subset behind
a startup transaction. It is RT0 configuration, application, and introspection;
it does not establish RT1 or RT2. NUMA memory placement, custom stacks/guards,
locking, pinning, huge pages, prefault, first touch, and residency remain
M15-03.
M15-01 does not call affinity, scheduling, NUMA, stack, locking, pinning,
huge-page, prefault, first-touch, or residency mutation APIs; native thread
application begins only in M15-02.

The model is separate from runtime-config schema 7 and stable C ABI v8.
`Runtime::set_cpu_memory_policy()` copies a bounded request while the runtime
is configuring. `finalize()` validates the request, resolves it against the
complete current resource inventory, and transactionally commits immutable
reports. A failed finalization leaves the runtime configuring.

## Thread policy

`ThreadResourceId` combines a stable `ThreadRole` numeric identifier with an
instance index. The roles are:

| Role | Inventory rule | Ownership |
| --- | --- | --- |
| `frame` | one calling/periodic lane | host, verify-only |
| `executor_worker` | one entry per configured worker | runtime for native policies; host and verify-only for `host_adapter` |
| `watchdog_service` | one entry when the watchdog is enabled | runtime |
| `device_service` | one entry when any device backend is registered | runtime |
| `xdma_io` | one entry per explicitly declared backend worker | backend, verify-only |
| `accelerator_submission` | extensible explicitly declared backend lane | backend, verify-only |

XDMA worker count is backend-private and is not present in device ABI v1, so
the host that constructs an XDMA backend declares those externally owned role
instances when it needs them in a runtime policy report. The runtime does not
infer a count from a backend name or storage estimate.

`ThreadPolicy` contains a 256-logical-CPU fixed bit set, scheduling class and
priority, NUMA node, wait strategy, stack and guard byte requests, and a
64-byte bounded name. Empty/default fields mean preserve the 1.2.1 behavior.
Names use the existing stable identifier character set and must be NUL
terminated.

M15-02 resolves Linux current-thread CPU affinity, normal/FIFO/round-robin
scheduling and priority, and names shorter than the native 16-byte buffer.
Runtime-owned executor workers also support `spin` and `yield`; the unchanged
default is `yield`. `park`, `adaptive`, NUMA-node, custom-stack, and guard
requests are unsupported in this batch. Best-effort requests drop unsupported
fields and report `portable_fallback`; required requests fail finalization.
Windows and other providers without a supported native operation use the same
fallback/strict rule.

`ThreadPolicyProvider` is an additive C++ boundary for deterministic tests and
host-specific current-thread providers. The default provider is native Linux
where compiled and a portable unsupported provider elsewhere. Every
runtime-owned executor, watchdog, and device-service lane invokes it on itself,
publishes readiness, and blocks on the runtime's atomic startup decision.
Commit releases all lanes only after required application and readback pass.
Abort releases them only to exit, then existing reverse cleanup joins the lanes
and shuts down initialized device backends before `start()` returns. A later
successful `start()` begins with fresh report and barrier state.

The frame lane is inspected on the calling thread and is never mutated.
Host-adapter workers and declared backend/vendor lanes expose no owned native
handle, so they remain `verify_only` and are never falsely reported as
runtime-owned. A required non-default request for an inaccessible external lane
fails finalization.

Reports retain `requested`, `resolved`, `applied`, and `verified` policy,
application/verification status, native system errors, and whether an applied
policy was rolled back by thread termination. Best-effort native failure does
not block startup but never reports a failed field as verified.

## Memory policy

`MemoryRegionId` combines a stable category, optional thread role for stack
entries, and an instance index. The inventory categories are:

| Category | Instances | Accounting |
| --- | --- | --- |
| `runtime_control` | one | `MemoryPlan::runtime_control_bytes` |
| `executor_control_and_queues` | one | `executor_control_bytes`, including native queues or host-adapter runtime records |
| `device_control_and_queues` | one, including zero bytes when unused | `device_control_bytes`, including outstanding and completion storage |
| `phase_scratch` | one aggregate | `phase_scratch_total_bytes` |
| `task_scratch` | one aggregate | `task_scratch_total_bytes` |
| `trace_storage` | one aggregate | `trace_storage_bytes` |
| `thread_stack` | one per thread inventory entry | excluded/informational until stack ownership is implemented |
| `backend_storage` | one per backend | backend-reported and excluded |
| `registered_state` | one per borrowed state span | host-owned and excluded |
| `registered_device_buffer` | one per borrowed buffer span | host-owned and excluded |

The first six keys partition `MemoryPlan::planned_bytes` exactly once. Every
thread and excluded region also receives one unique accounting key, but an
excluded entry never contributes to `runtime_accounted_bytes`. Stack sizes are
reported as zero under portable defaults because the current implementation
does not create or inspect custom stack storage.

Excluded keys identify logical registrations, not deduplicated physical RSS.
For example, a host span separately registered as canonical state and as a
device buffer has two ownership contracts and two excluded keys. M15-01 does
not promote their informational byte sum into runtime-plan accounting.

`MemoryRegionPolicy` represents provider ownership, alignment, page rounding,
before/after guards, prefault, locking, pinning, huge-page preference and
fallback, NUMA placement, first touch, residency verification, and rollback
intent. The report keeps requested, resolved, applied, and verified fields
separate. M15-01 does not change the allocator or committed region layout.

## Validation

Finalization rejects:

- invalid enum values, CPU bits outside the declared logical-CPU range,
  unterminated/invalid names, invalid alignments, and arithmetic overflow;
- duplicate thread IDs or memory-region IDs;
- scheduling priorities that contradict their scheduling class, guards
  without stack/page ownership, and invalid huge-page fallback combinations;
- provider, first-touch, or rollback requests that contradict resolved
  ownership;
- requests for roles or regions absent from the finalized inventory, except
  explicitly declared backend-owned XDMA/accelerator roles;
- more than 1,024 thread or memory overrides;
- `required` thread fields unsupported for their owner or provider, including
  M15-03 custom-stack/guard and NUMA requests;
- `required` memory policy requests, because memory application and
  verification remain M15-03.

Rejection of unsupported required policy is intentional fail-closed behavior.
Supported M15-02 thread fields are accepted only when the selected provider can
apply and read them back before callbacks.

## Compatibility and claim boundary

The change is additive to the C++ source API. Runtime-config schema 7, profile
parsing, stable C ABI v8, SONAME 8, device ABI v1, installed header inventory,
package components/targets, compatibility aliases, and Apache-2.0 remain
unchanged. Callback dispatch does not consult the reports; only an executor
worker's finalized spin/yield choice affects its idle loop.

Portable and injected tests validate model behavior, inventory, ownership,
startup exclusion, failure rollback/retry, and runtime-instance isolation. An
unprivileged Linux test applies and reads back one worker's allowed affinity,
normal scheduler, and name. Privileged FIFO/RR success, custom stacks, memory
residency, hardware behavior, latency, RT1, and RT2 remain unperformed.

# CPU and memory policy model

M15-01 adds the C++ policy and report model used by later M15 startup and
memory-provider batches. It is portable RT0 configuration and introspection.
It does not call affinity, scheduling, NUMA, stack, locking, pinning,
huge-page, prefault, first-touch, or residency mutation APIs. Configuring a
policy or passing portable tests does not establish RT1 or RT2.

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

M15-01 resolves defaults to the current behavior. A non-default best-effort
request is retained as `requested` while `resolved` remains the portable
default and the resolution state is `portable_fallback`. Application is
`not_performed`. Host- and backend-owned roles report verification as
`verify_only`; runtime-owned roles report verification as `not_performed`.
Later M15 batches populate the already-distinct applied and verified stages.

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
- `required` requests, because native application and verification are not
  implemented in M15-01.

Rejection of a required policy is intentional fail-closed behavior. A later
batch may accept it only after the named platform provider can apply, verify,
and roll back the complete request before callbacks.

## Compatibility and claim boundary

The change is additive to the C++ source API. Runtime-config schema 7, profile
parsing, stable C ABI v8, SONAME 8, device ABI v1, installed header inventory,
package components/targets, compatibility aliases, and Apache-2.0 remain
unchanged. No callback or steady-state path consults these reports in M15-01.

Portable tests validate model behavior, arithmetic, inventory, ownership, and
runtime-instance isolation only. Native policy application, OS verification,
memory residency, rollback, hardware behavior, latency, RT1, and RT2 remain
unperformed.

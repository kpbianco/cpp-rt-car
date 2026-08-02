# Bounded Device Backend Contract

RTFW 1.2 retains the M8 target `rt::Runtime` device contract introduced in
0.9. The base contract adds a
size/versioned C backend ABI, device phases in the compiled graph, one
runtime-owned completion-service lane, and a deterministic fault-injectable
CPU mock. This base is portable RT0 functional behavior. It is not itself
CUDA, Vulkan, XDMA, driver, latency, or RT2 qualification. The optional M9
CUDA implementation and its separate hardware-evidence boundary are in
[the CUDA backend contract](cuda_backend.md). The M10 Xilinx Linux XDMA AXI-MM
candidate uses the same device ABI while isolating blocking character-device
calls on fixed backend workers; its separate boundary is in
[the XDMA backend contract](xdma_backend.md).

The legacy `hal/gpu_stub.hpp` detached-thread experiment is unchanged and
outside this contract. `rt::Runtime` does not invoke it.

## Ownership and lifecycle

A host registers a `rtfw_device_backend_api` while the runtime is configuring.
The function table and its `instance` are borrowed through backend
`shutdown()`. Registered buffer storage and device-phase `user_data` are also
borrowed until runtime shutdown has completed. The runtime copies names,
capabilities, graph metadata, and the function table; it does not take
ownership of backend or application storage.

The target lifecycle is:

1. `register_device_backend()` validates ABI version 1, every required
   function, reserved fields, identifiers, and reported capacities.
2. `register_device_buffer()` records a nonempty, nonoverlapping host span and
   access flags. The backend does not see it yet.
3. `register_device_phase()` adds a command-provider phase to the same graph as
   CPU phases.
4. `finalize()` validates backend limits and commits manager, outstanding-slot,
   completion-batch, and graph storage to the memory plan.
5. `start()` starts the fixed CPU team, initializes each backend, registers
   buffers, then starts one runtime-owned device service lane. In M15-02 that
   lane applies and reads back its thread policy, reports readiness, and waits
   for the runtime-wide startup commit before polling a backend. Failure performs
   a checked reverse rollback. An initialization failure is
   ownership-uncertain until one `shutdown()` attempt returns `OK` or
   `INVALID_STATE`; any other cleanup result is retained for `stop()` retry.
   M15-03 may create the service stack through the memory-region provider;
   failure joins the service before returning that stack and then preserves
   this same checked backend cleanup order. Backend storage, buffers, and
   vendor I/O stacks remain external/verify-only.
6. `step()` invokes a command provider on a CPU worker. The provider fills one
   fixed-size submission and returns. Accepted device work retains the phase's
   graph completion token; independent CPU work remains runnable.
7. The service lane polls completions and releases dependent graph phases.
8. `stop()` joins the service lane, cancels unresolved graph tokens,
   unregisters buffers in reverse order, and shuts backends down in reverse
   order before the CPU executor and watchdog finish stopping. Successful
   operations clear their ownership markers. Failed operations retain them;
   a backend is never shut down while one of its buffers remains registered,
   and independent cleanup continues while the first failure is preserved.
   Repeated `stop()` calls invoke only the unresolved operations. No backend
   callback exists in the ABI, so backend code cannot call into destroyed
   runtime or plugin ownership.

A required device-service thread policy failure aborts the barrier before the
first poll and invokes this same checked reverse buffer/backend cleanup. If
cleanup succeeds, the runtime remains finalized and a later `start()` may
retry; a cleanup failure retains the existing explicit stop-pending ownership.

Runtime control methods are single-host-thread operations. The host must not
destroy a runtime, backend instance, buffer, plugin, or callback data while a
host call or callback is active.

If cleanup fails, `stop()` returns that status and leaves the public lifecycle
state at `running` or `finalized`; execution, restore/replay, health, reset, and
restart operations are gated until a later `stop()` succeeds. The service
lanes and executor are already quiesced. The host must retain every borrowed
backend table, instance, buffer, plugin, and callback object throughout the
retry. A C++ destructor cannot report a cleanup result and is only a
best-effort fallback, so device integrations must use the checked `stop()`
path.

## ABI shape

`rt/include/rt/device_abi.h` is a C-compatible backend boundary with device ABI
version 1. All extensible records carry `struct_size`; ABI-bearing records also
carry `abi_version`. Reserved fields must be zero.

The backend supplies:

- capability discovery and initialization;
- host-buffer registration and unregistration;
- bounded, nonblocking submission;
- bounded, nonblocking completion polling;
- cancellation support or an explicit `unsupported` result;
- health, reset, and shutdown.

Every function is required in the table and must not throw across the ABI.
Optional behavior is represented by a capability byte and a stable status, not
by a null function. `submit()` and `poll()` must return without sleeping,
waiting for device completion, allocating an unbounded spill queue, invoking
host code, or creating a per-submission thread. `poll()` writes only to the
caller-provided completion array and never invokes a callback.

After an `initialize()` attempt fails, the runtime calls `shutdown()` once.
The backend returns `INVALID_STATE` only when that failed initialization owns
no resource requiring release. If partial initialization or rollback retained
anything, `shutdown()` returns the cleanup failure and remains retryable; it
must not report `INVALID_STATE`. The runtime then retains the backend table and
instance until a later shutdown succeeds. The same retry rule applies after a
failed buffer unregistration or ordinary shutdown.

`submit()` and `poll()` may run concurrently for one initialized backend
instance. A backend must synchronize its own fixed-capacity state without
blocking either caller. The manager uses an explicit submitting/early-ready
handshake: successful accounting and `device.submitted` are published before
an early completion is applied, so a completion cannot overtake its submission
record.

The current fixed submission contains:

- runtime-assigned submission and frame IDs;
- a positive backend-enforced `timeout_ns`;
- backend-defined opcode and up to 128 inline payload bytes;
- up to eight buffer references with token, access, offset, and length;
- zero flags and reserved fields in ABI version 1.

Application command providers use runtime-local logical buffer handles. The
manager bounds-checks each range and access, verifies backend ownership, and
translates it to the backend's private token before submission.

## Execution semantics

A device phase has two distinct moments:

- the command provider is a normal bounded CPU phase and ends when submission
  is accepted or rejected;
- the graph phase completes only when the service lane publishes its device
  completion.

CPU compute workers never wait for a device fence, future, callback, or
condition variable. `step()` remains synchronous to its host: it returns after
all CPU and device graph tokens have completed or one has failed. A dependent
phase is never released by provider return alone. Independent phases may run
while a device is outstanding.

The device service lane uses preallocated outstanding slots and a preallocated
completion batch. It sleeps on an atomic notification when there is no device
work and polls only while submissions are outstanding. Backend polling
frequency and hardware completion bounds remain backend/deployment concerns.

## Status and recovery

Backend statuses map to stable runtime statuses:

| Device ABI result | Runtime result |
| --- | --- |
| `QUEUE_FULL` | `device_queue_full` |
| `TIMEOUT` | `device_timeout` |
| `ERROR`, `UNSUPPORTED`, `INTERNAL_ERROR` | `device_error` |
| `LOST` | `device_lost` |
| `CANCELED` | `device_canceled` |
| `RESET_REQUIRED` | `device_reset_required` |
| `RESOURCE_EXHAUSTED` | `resource_exhausted` |

`device_health()` and `reset_device()` are non-RT host operations. They require
a running runtime, reject an active frame/periodic/replay call, validate the
instance-local backend handle, and never race an outstanding runtime
submission. Reset produces a trace event and counter only after the backend
reports success.

## Deterministic mock

`rt::MockDeviceBackend` owns fixed queue and buffer tables and creates no
thread. The runtime's service lane advances it by polling. Accepted
submissions have a one-based deterministic ordinal. A pre-start fault script
can attach one action to an ordinal:

- `delay`: add a fixed number of polls before completion;
- `timeout`: complete with timeout after deterministic timeout/poll-quantum
  conversion;
- `error`: complete with a stable backend error;
- `loss`: complete as lost and require reset.

The mock also supports deterministic queue saturation, inline writes, byte
fills, cancellation, health counters, reset generations, and shutdown. It
declares `deterministic_mock=1`; D1 finalization rejects any registered backend
that does not make that declaration. This declaration qualifies only the mock
model and application obligations in the D1 contract. It does not make real
hardware deterministic.

## Memory and observability

The finalized memory equation now includes `device_control_bytes`. The plan
reports backend/buffer counts, outstanding capacity, completion batch, and
backend-reported private control bytes. Backend-reported storage is
informational and excluded from `planned_bytes` because the backend owns it.
Registered buffer payload bytes are borrowed and excluded.

Observability schema version 2 retains IDs 0–21 and adds device submission,
completion, failure, saturation, timeout, loss, reset, poll, outstanding, and
service-start metrics. Trace IDs 12–14 report submission from its CPU worker,
completion from `device_service`, and reset from `host`. Telemetry is
diagnostic and does not establish a completion deadline.

## Evidence and exclusions

Automated evidence covers queue saturation, cancellation, delayed dependency
release, timeout, error, loss, reset, reverse/retryable shutdown, failed-start
rollback, malformed tables, C ABI dynamic loading, memory-plan accounting,
steady-state allocation, and sanitizer execution in
`tests/test_device_runtime.cpp`, `tests/test_trace_noalloc.cpp`, and
`tests/test_cabi_dlopen.c`. `samples/device_mock.cpp` is the minimal C++ flow.

M9 remains separately gated even though 1.2 contains a real CUDA Driver API
adapter candidate. It still needs a named driver, toolkit, hardware, OS, and
workload support tuple plus functional, recovery, resource, and
latency-decomposition evidence before its support matrix can contain a
qualified tuple. M10 likewise provides a bounded XDMA candidate but no
qualified hardware tuple. Neither backend inherits a portable completion-time
claim from M8.

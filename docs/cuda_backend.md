# CUDA Backend Candidate and Qualification Contract

RTFW 1.2 retains the M9 CUDA Driver API backend candidate introduced in 0.10. The backend is a
real vendor adapter, not the legacy detached-thread GPU stub and not the M8 CPU
mock. Its implementation and CPU-only state-machine evidence are present, but
the repository has no approved hardware support tuple. M9 therefore remains a
candidate rather than a completed or qualified milestone.
There is no qualified tuple in the 1.2 support matrix.

## Claim boundary

The implementation establishes these framework properties:

- the core scheduler still depends only on device ABI version 1;
- the host supplies a live `CUcontext` and one or more live `CUstream` handles;
- initialization pre-creates one `CUevent` per fixed in-flight slot;
- buffer, event, stream, submission, and kernel registries have configured
  capacities;
- `submit()` and `poll()` create no thread, perform no explicit device wait,
  and allocate no framework memory;
- completion uses event query rather than event synchronization;
- shutdown is a non-RT host operation and drains outstanding CUDA work before
  releasing backend-owned registrations and allocations.

That drain-before-release rule is a memory-lifetime requirement, not a latency
claim.

These properties do not establish a host-call or device-completion deadline.
CUDA can perform lazy initialization, JIT compilation, page-table work,
firmware work, recovery, or other vendor-controlled operations inside an API
call. A supported tuple needs warm-up plus measured functional, resource,
failure, and latency evidence. No RT1, RT2, worst-case, or generic
real-time-GPU claim is made.

The machine-readable boundary is
[the CUDA support matrix](cuda_support_matrix.json).

## Targets and package surface

`rtfw::cuda_backend` is always built and installed. It contains the bounded
state machine and an injectable `rt::CudaDriverApi`; it has no CUDA toolkit
dependency. This allows ordinary Linux and Windows CI, sanitizers, and
ThreadSanitizer to exercise CUDA scheduling semantics with a deterministic
fake driver.

`RTFW_ENABLE_CUDA=ON` requires CUDAToolkit and additionally builds and exports:

- `rtfw::cuda_driver`, the production adapter linked to
  `CUDA::cuda_driver`;
- `sample_cuda_qualification`, the opt-in functional and evidence executable.

An installed package exposes `RTFW_WITH_CUDA_DRIVER` and resolves CUDAToolkit
only when the package was built with the adapter. The CUDA implementation does
not change the core C ABI or device ABI.

## Host-owned lifecycle

The host must:

1. initialize the CUDA Driver API;
2. retain or create a context and create the streams;
3. load modules and obtain every `CUfunction`;
4. construct `rt::CudaDeviceBackend` with opaque context/stream handles;
5. register kernel handles and optional external device allocations before
   runtime start;
6. keep the context, streams, modules, functions, host buffers, and external
   device allocations alive through backend shutdown;
7. destroy CUDA resources only after runtime/backend shutdown returns.

The backend pushes and pops the supplied context around each setup, submit,
poll, reset, and shutdown interaction. It never creates a context, stream,
module, or worker thread, so an engine can retain ownership of its established
CUDA environment.

Kernel registration returns a stable, one-based token. Device buffer
registration also returns a private token; the runtime translates its logical
buffer handle before calling the backend.

## Buffer ownership and coherence

Every runtime buffer remains a borrowed host span. By default, CUDA
registration at runtime start performs two non-RT setup actions:

- register the borrowed host span as page-locked memory;
- allocate an equal-size backend-owned device mirror.

Shutdown or buffer unregistration reverses both actions. Host registration can
fail because of permissions, alignment, overlap, driver limits, or locked
memory limits; startup then fails explicitly.

An engine can bind a buffer name to an existing `CUdeviceptr` with
`bind_device_buffer()`. The engine retains that allocation. It can also set
`register_host_memory=false` when the host span is already page locked and its
lifetime is managed externally. Setting `allocate_device_mirrors=false`
requires a binding for every registered buffer.

There is no implicit coherence. The command stream must explicitly submit a
host-to-device or device-to-host copy at the correct graph boundary. A
registered span must not be read, written, moved, unpinned, or freed contrary
to its declared access while CUDA work can reference it.

## Opcodes

| Opcode | Submission shape | CUDA operation |
| --- | --- | --- |
| `cuda_device_opcode_noop` | No payload or buffers | Records a completion event after earlier work on the selected stream |
| `cuda_device_opcode_copy_host_to_device` | One readable buffer reference | Async copy from the borrowed host span to its device mirror |
| `cuda_device_opcode_copy_device_to_host` | One writable buffer reference | Async copy from the device mirror to the borrowed host span |
| `cuda_device_opcode_copy_device_to_device` | Read source and write destination references of equal size | Async device copy |
| `cuda_device_opcode_memset_d8` | One writable reference and one-byte payload | Async byte fill |
| `cuda_device_opcode_launch_kernel` | Fixed launch payload and up to eight references | Driver API kernel launch |

Each copy uses the reference offset and byte count on both the host span and
device mirror. Ranges and access flags are validated before the driver call;
overlapping device-to-device ranges within one allocation are rejected rather
than delegated to undefined driver-copy behavior. Queue exhaustion returns
`RTFW_DEVICE_STATUS_QUEUE_FULL`; it never creates a spill queue or performs
an inline wait.

## Fixed kernel payload

`rt::CudaKernelLaunch` occupies the device ABI's complete 128-byte inline
payload. It contains:

- a pre-registered kernel token;
- three-dimensional grid and block dimensions;
- dynamic shared-memory bytes;
- up to eight fixed descriptors;
- up to 48 inline scalar bytes;
- zeroed reserved fields.

A descriptor is either a buffer address or a scalar of one through eight
bytes. Buffer descriptors reference the submission's buffer table, so the
backend derives a checked `CUdeviceptr` instead of accepting an unchecked
address in the inline payload. Scalar and device-address argument storage is
copied into aligned fixed stack arrays before launch. Module/function
ownership remains with the host.

## Concurrency and streams

Slots are claimed with bounded atomic compare/exchange attempts. A claimed
slot uses its preassigned stream and event. Multiple submitters may call
`submit()` concurrently with the runtime's single completion poller; each slot
has independent state and no blocking framework mutex.

Streams are assigned to slots round-robin. CUDA stream ordering governs work
within one stream; separate streams may overlap when the device and host
configuration permit it. RTFW graph dependencies remain the application-level
ordering contract. The backend does not infer cross-stream hazards beyond
validated buffer ranges and declared graph order.

## Timeout, cancellation, and recovery

CUDA work already enqueued in a stream cannot be safely canceled through this
backend. `cancel()` therefore returns `UNSUPPORTED` and the capability reports
no cancellation support.

When host monotonic time reaches `submission.timeout_ns` while an event is
still not ready, the slot is marked timed out but is not completed or reused.
Polling continues with nonblocking event queries. Only after the event
physically completes does the backend publish `TIMEOUT` and release the slot.
This quarantine rule prevents the runtime from releasing a graph dependency,
host span, device allocation, module, or stream while timed-out work could
still access it. A permanently hung driver can therefore prevent synchronous
`step()` or shutdown from returning; that deployment failure cannot be solved
by the portable backend without violating memory lifetime.

An enqueue or event-record failure after an operation was attempted also
quarantines its slot and moves health to `RESET_REQUIRED`. Non-RT `reset()`
synchronizes affected streams before reuse. A reported context loss completes
accepted submissions as `LOST`, moves health to `LOST`, and cannot be repaired
by soft reset. The uncertain backend slot remains quarantined even though the
runtime receives the terminal `LOST` result. The host must stop, recreate its
CUDA context/resources, and construct a new backend. Shutdown may explicitly
wait because it is a host lifecycle operation whose safety obligation is to
drain before release. If a shutdown drain or cleanup call fails, the backend
releases no resource whose ownership is still uncertain, rejects ordinary
operations, and permits `shutdown()` to be retried. The host must not destroy
borrowed host storage, streams, modules, or the context until shutdown
succeeds; an unrecoverable context loss requires context destruction as the
final reclamation boundary.

## Evidence procedure

CPU-only automated evidence covers:

- malformed driver/configuration and registration rejection;
- fixed-capacity saturation and concurrent submitters;
- host/device and device/device transfer paths;
- fixed-payload kernel argument construction and launch;
- timeout quarantine until physical readiness;
- enqueue-failure quarantine and drain-before-reset;
- context-loss mapping and soft-reset refusal;
- drain-before-free shutdown;
- runtime graph integration at D0;
- zero framework allocation in steady-state submit/poll;
- AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer paths.

The optional hardware executable runs a real upload, PTX kernel, and download,
validates every result, discards an explicit warm-up interval, and emits JSON
containing the warm-up and measurement counts, driver/toolkit/device/PCI
identity, compute capability, health counters, and raw measured per-stage:

- submission host-call duration;
- completion wait duration;
- accumulated poll host-call duration;
- poll count.

Run it locally:

```bash
cmake -S . -B build-cuda \
  -DRTFW_ENABLE_CUDA=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-cuda \
  --target simcore_tests sample_cuda_qualification \
  --parallel 2
GTEST_FILTER='CudaBackend.*' \
  ctest --test-dir build-cuda --output-on-failure -R simcore_all
./build-cuda/samples/sample_cuda_qualification \
  --warmup 1000 \
  --iterations 10000 > cuda-qualification.json
```

The `CUDA qualification` workflow performs the same opt-in process only on a
self-hosted runner labeled `nvidia-gpu`. It validates the schema as
`evidence_only`, records the JSON byte length and SHA-256 against the complete
source commit, verifies that manifest, and uploads both files. A passing run is
evidence for review, not an automatic support-matrix mutation.

## Qualification exit gates

M9 can move from Candidate to Complete only after at least one declared tuple
has:

- exact GPU, PCI topology, OS, kernel, CUDA driver, toolkit, compiler, runtime
  build, power policy, clocks, and workload identity;
- a clean adapter build plus all CPU-only backend tests;
- repeated functional transfer/kernel validation;
- warm-up and a steady-state run with stable event, device allocation, and
  pinned-host resource counts;
- injected or naturally reproduced failure/recovery evidence;
- raw latency-decomposition samples and thresholds selected before the run;
- an explicit pass/fail review committed to the versioned support matrix.

No tuple has completed those gates in the repository at 1.2.0.

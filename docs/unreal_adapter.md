# Unreal adapter candidate

M19-02 adds an opt-in, source-distributed Unreal Engine adapter candidate for
one approved source tuple: Unreal Engine tag `5.8.1-release` at commit
`71fe36aac5a8df5ccd66c763ffc902b29b6a9c43` on Debian 12 x86-64 with Epic's
bundled `v26_clang-20.1.8-rockylinux8` Clang, libc++, and lld SDK. The plugin is
not a default CMake target, installed RTFW header, package component, release
artifact, or compatibility promise for another engine version.

The adapter is portable RT0 functional integration only. An Unreal build,
editor automation run, allocator observation, or functional performance budget
does not establish packaged-game behavior, world lifecycle, module unload or
hot reload, absence of engine-internal allocation, worst-case latency, RT1,
RT2, hardware or HIL qualification, signing, release, deployment, or production
readiness.

## Passive module and ownership

`RTFWUnreal` has no startup or shutdown callback. Loading the module constructs
no `rt::Runtime`, registers no world, engine, tick, or reload delegate, starts
no worker, and creates no process-global Runtime registry. Hosts explicitly
construct one job adapter, memory provider, and clock per Runtime while the
Runtime is configuring.

The host must preserve this order:

1. construct and reserve adapter storage;
2. configure the Runtime and attach the copied host-executor and memory-provider
   tables;
3. finalize, start, and use host-driven `step()` calls;
4. close job admission, call checked `Runtime::stop()` until it succeeds, and
   verify the engine task nodes are quiescent;
5. destroy the Runtime before the borrowed adapter objects.

M19-02 does not detach an extension, unload an operating-system module, own a
world, or orchestrate concurrent worlds. M19-03 and M19-04 retain those
responsibilities. Destroying or unloading the module while a Runtime, accepted
task node, or provider token is live violates the ownership contract.

## Job adapter

`FRTFWUnrealJobAdapter` allocates a power-of-two array of job slots before the
Runtime starts. Each slot contains one copied `rt::HostExecutorJob`, one
generation-tagged atomic state word, and one embedded engine task node. The
adapter exposes exactly the configured logical worker count and total queue
capacity. A logical worker index is bounded Runtime metadata, not an Unreal
thread identifier or an observation of affinity, priority, stack, or physical
worker cardinality.

Submission makes one bounded pass over the reserved slots. It does not retry
until success, wait, spill, invoke a rejected job inline, allocate adapter-owned
storage, or create a thread. A successful submission transfers one copied job
to the engine scheduler. Capacity or injected prelaunch rejection returns
`queue_full` only after proving the job was not invoked. Closing admission makes
later submissions fail without acceptance.

The accepted, executing, awaiting-retirement, and free states prevent reuse
until the RTFW execute callback has returned and the engine reports the embedded
task complete. Generation exhaustion closes admission rather than wrapping.
The one-job help callback considers at most one accepted task and uses engine
expediting; it does not wait for or drain arbitrary engine work.

The sole D-009 exception is the exact-version use of
`Async/Fundamental/Task.h` for `LowLevelTasks::FTask` and its task-node
operations, plus `Async/Fundamental/Scheduler.h` for
`LowLevelTasks::TryLaunch`. At the pinned commit, `FTask::IsCompleted()` is the
documented recycle test and `FTask::TryExpedite()` does not permit reuse until
the scheduler has released its reference. No engine Private header, task graph
implementation, scheduler queue implementation, or worker-control API is used.

Unreal owns scheduler queues, workers, task-system synchronization, and any
engine-internal allocation. Adapter slot bytes are reported separately. Engine
allocation and timing remain verify-only facts for the named tuple and cannot
be converted into a no-allocation or RT claim.

## Memory provider

`FRTFWUnrealMemoryProvider` supplies the existing memory-provider API version 1
for exactly phase scratch, task scratch, and trace storage. It uses public
`FMemory::Malloc`, `FMemory::GetAllocSize`, and `FMemory::Free` operations with
the Runtime's checked size and power-of-two alignment. Each successful token is
one adapter-owned record and one nonoverlapping allocator extent.

The provider advertises policy operations and allocator-extent observation
only. It rejects guard pages, base-page rounding, explicit huge-page preference,
prefault, locking, pinning, explicit first touch, NUMA placement, and residency
verification. A second `GetAllocSize` result verifies the allocator-reported
extent but is not an operating-system readback and does not establish physical
commitment, residency, locking, device or DMA pinning, NUMA placement, huge
pages, or allocator-overhead exactness.

Apply attempts are rollback-eligible before success is known. Failed apply or
observe operations retain enough state for Runtime rollback; failed rollback
retains ownership for a later checked-stop retry. Release frees only a live
owned token, once, after successful rollback, and Runtime releases trace, task,
then phase storage in reverse acquisition order.

## Clock and frame mapping

`FRTFWUnrealClock` samples public `FPlatformTime::Cycles64()` and the matching
`GetSecondsPerCycle64()` conversion. Each instance clamps its own nanosecond
result to a monotonically nondecreasing value. Conversion rejects nonfinite,
negative, nonpositive-when-required, precision-range, and overflow cases and
uses round-half-up nanoseconds without floating accumulation or a process-global
epoch.

The clock intentionally does not own engine sleep:
`supports_absolute_sleep()` is false and `sleep_until_ns()` returns
`clock_failure`. It is valid for host-driven `Runtime::step()` and cannot pace
or qualify `Runtime::run_periodic()`.

`FRTFWUnrealFrameContext::Make()` preserves an explicit engine frame sequence,
rounds a positive finite simulation delta to nanoseconds, and optionally maps
deadline and nominal-release cycle values through the same clock conversion.
It does not derive simulation delta, invent a frame counter, sleep, or mix world,
audio, network, and monotonic clock domains. Invalid input leaves the caller's
output unchanged.

## Build, tests, and evidence boundary

The plugin links a separately configured and relocated `rtfw_runtime` static
archive from the exact target revision. RTFW and Unreal must use the same
architecture, compiler, libc++, lld, exception model, and compatible build
configuration. RTFW sources are not compiled into the Unreal module, and the
experimental plugin manager is not used.

`scripts/verify-unreal.sh` validates the engine commit, exact tag and version,
bundled compiler and linker versions, and the compiler identity recorded in the
runtime archive before staging the source plugin into the fixture. The approved
workflow builds Development editor and game targets, a Shipping game target,
runs `RTFW.M19_02` editor automation, and retains command logs, automation
reports, manifests, tuple identity, and the runtime archive digest.

The automation covers configuration rejection, exact-once execution,
saturation, helping, task retirement, stale dispatch, generation exhaustion,
allocator acquire/apply/observe failure and rollback/retry, unsupported policy
truth, checked time conversion, host-driven lifecycle, callback failure, and
two-instance isolation. These results remain tuple-local functional evidence.

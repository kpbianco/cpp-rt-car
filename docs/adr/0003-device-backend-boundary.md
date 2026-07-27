# ADR-0003: Devices use a bounded backend ABI

- Status: Accepted
- Date: 2026-07-23
- Implementation milestone: M8 complete; M9 CUDA candidate awaiting hardware
  qualification; M10 XDMA remains

## Context

The current HAL wraps standard allocation, threads, futures, and clocks. Its
`pinned` and `hugepage` flags are no-ops. The GPU mock creates a detached CPU
thread per submission, and there is no XDMA implementation.

Embedding environments may already own a GPU context, command queue, or device
service. Device code also has failure and lifetime rules that do not belong in
the CPU executor.

## Decision

The runtime core depends on a size/versioned device backend contract rather
than a vendor API. A backend defines:

- capability discovery and initialization;
- buffer registration and ownership;
- bounded, nonblocking submission;
- completion polling/publication;
- timeout and cancellation where supported;
- health, reset, and shutdown;
- stable error and telemetry records.

Device submissions participate in the compiled resource graph. CPU workers do
not block waiting for a device; completions release dependent work through a
dedicated service/completion lane.

Plugins receive size/versioned host services and registration tables. Plugins
are loaded before finalization and are not unloaded while any callback,
resource, or submission can reference them.

## Consequences

- A deterministic fault-injectable mock backend is implemented before CUDA or
  XDMA.
- CUDA accepts an explicit host-owned context/stream set or clearly owns one.
- XDMA support names one concrete driver/device contract and FPGA test image.
- API boundedness and hardware completion qualification are documented
  separately.
- The target runtime creates no detached submission thread. The legacy GPU
  stub remains an explicitly excluded compatibility experiment.
- The M9 implementation uses the host-owned option: the host retains its CUDA
  context, streams, modules, functions, and any externally bound allocations.
  A versioned support tuple remains separate from implementation presence.

## Rejected alternatives

- Put CUDA/XDMA calls directly in the graph core. This couples lifecycle,
  packaging, and error semantics to one vendor stack.
- Treat a future as the backend ABI. It hides queue capacity, ownership,
  timeout, health, and reset behavior.
- Claim generic GPU/XDMA real time. Completion bounds are deployment-specific.

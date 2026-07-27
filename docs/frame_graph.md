# Device Frame-Graph Mock

`gpu/frame_graph.hpp` is a CPU-only experiment for device-style dependency and
completion APIs. It is not a GPU backend.

Current behavior:

- “GPU” callbacks execute on detached CPU threads from `hal/gpu_stub.hpp`.
- fences use `std::promise`/`std::shared_future`;
- waits are handed to a `FiberPool`, which is another CPU execution component;
- timeline semaphores are atomic counters with busy waits;
- resources use a 64-byte-aligned host allocation;
- the HAL `pinned` and `hugepage` flags are currently ignored;
- SPIR-V words and CUDA labels are accepted by wrappers but no shader/module is
  loaded and no device work is submitted;
- resources are freed after their recorded last pass during `execute()`;
- overlap is an observed wall-clock estimate, not a device scheduling budget.

This mock is useful only for API experiments and unit tests. Detached thread
creation, future allocation, busy waits, unchecked resource indices, and
implicit ownership all violate the target device contract.

Milestone M8 supersedes this experiment for `rt::Runtime` with the bounded,
fault-injectable path in the
[device backend contract](device_backend.md). This legacy experiment remains
for compatibility. The separately linked M9 CUDA and M10 XDMA candidates
implement the target contract, but neither makes this legacy experiment a
hardware backend or establishes a qualified hardware tuple.

## Code anchors

- Frame-graph experiment: `simcore::hal::gpu::FrameGraph`;
  `gpu/frame_graph.hpp`
- CPU submission mock: `simcore::hal::gpu::submit`; `hal/gpu_stub.hpp`
- Allocation flags: `simcore::hal::MemFlags`; `hal/hal.hpp`
- Target boundary: [ADR-0003](adr/0003-device-backend-boundary.md),
  [device backend contract](device_backend.md)

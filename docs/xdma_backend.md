# Xilinx XDMA Backend Candidate and Qualification Contract

RTFW 0.11 contains the M10 XDMA backend candidate for one deliberately narrow
stack:

- x86-64 Linux;
- the official `Xilinx/dma_ip_drivers` XDMA Linux character driver, anchored
  for implementation review at commit
  `8721136e74a66500b02d16cb41922d966139cd46`;
- the PG195 XDMA AXI memory-mapped example design or a host design preserving
  its H2C/C2H address and data-integrity contract;
- channel-zero `/dev/xdma0_h2c_0` and `/dev/xdma0_c2h_0` endpoints by default;
- page-aligned host buffers and page-multiple transfers in the committed
  qualification procedure.

The portable backend state machine and Linux adapter are implemented, but no
hardware tuple is approved. M10 therefore remains Candidate. This is not a
generic FPGA/DMA backend and does not cover QDMA, AXI streaming, Windows,
vendor runtime libraries, arbitrary bitstreams, or kernel-bypass descriptor
ownership.

## Claim boundary

The implementation establishes framework-level properties:

- the scheduler continues to depend only on device ABI version 1;
- `submit()` validates and publishes into a fixed-capacity queue without
  calling the XDMA driver, waiting, allocating, or creating a thread;
- `poll()` inspects fixed slots and never invokes a host callback or blocking
  transfer;
- a fixed worker team created during backend initialization owns all blocking
  `pread()`/`pwrite()` calls;
- registered buffers, transfer slots, workers, and channel counts have
  configured capacities;
- H2C/C2H ranges, access flags, channel indexes, offsets, lengths, overflow,
  and optional alignment are validated before queue publication;
- shutdown is a non-RT host lifecycle operation that joins the fixed I/O team
  before borrowed buffers or channel descriptors may be released.

These properties do not bound Linux scheduling, page pinning, PCIe completion,
interrupt delivery, IOMMU behavior, driver allocation, FPGA response, or the
duration of a blocking character-device call. The official character driver
may pin pages or allocate internal request state per call. Consequently, the
candidate is RT0 functional infrastructure until a named deployment publishes
resource and latency evidence. It makes no RT1, RT2, worst-case, or portable
XDMA claim.

The machine-readable boundary is
[the XDMA support matrix](xdma_support_matrix.json).

## Targets and package surface

`rtfw::xdma_backend` is always built and installed. It contains the portable
fixed-capacity state machine plus injectable `rt::XdmaDriverApi`; ordinary
Linux and Windows builders need no vendor headers or device.

`RTFW_ENABLE_XDMA=ON` is supported only on Linux and additionally builds and
exports:

- `rtfw::xdma_linux`, the production character-device adapter;
- `sample_xdma_qualification`, the opt-in functional and raw-evidence tool.

The adapter uses only the documented character-device behavior exercised by
the upstream `dma_to_device` and `dma_from_device` tools. It does not copy or
link upstream driver code.

## Host-owned lifecycle

The host must:

1. load a compatible `xdma` kernel module and program the declared bitstream;
2. verify that the expected H2C/C2H nodes correspond to the intended PCI
   function and channel;
3. construct `rt::LinuxXdmaDriver` with immutable endpoint paths;
4. construct `rt::XdmaDeviceBackend` with matching H2C/C2H channel counts,
   transfer limit, buffer limit, worker count, and alignment;
5. register borrowed host buffers through the runtime;
6. keep the driver object, backend object, device nodes, bitstream, PCI
   function, and borrowed buffers stable through shutdown;
7. unload/rebind the driver or reprogram hardware only after shutdown returns.

Initialization opens every configured endpoint with `O_RDWR | O_CLOEXEC` and
creates the fixed I/O workers. Buffer registration does not allocate a bounce
buffer or pin memory itself. The host is responsible for any explicit
`mlock()` policy and must include that policy in qualification evidence.

## Submission contract

Both opcodes use one `rt::XdmaTransfer` payload and one registered-buffer
reference:

| Opcode | Required access | Operation |
| --- | --- | --- |
| `xdma_device_opcode_host_to_card` | Host-readable, device-writable reference | `pwrite()` to the selected H2C node at `device_offset` |
| `xdma_device_opcode_card_to_host` | Host-writable, device-readable reference | `pread()` from the selected C2H node at `device_offset` |

`XdmaTransfer::channel` selects the direction-specific channel. The backend
checks the buffer slice, device-address addition, configured maximum transfer,
and configured power-of-two alignment. The Linux adapter retries `EINTR` and
continues correct short transfers; a zero-length or terminal short transfer is
an error. A successful completion reports the physical byte count in
`rtfw_device_completion::value`.

The named qualification profile uses one 4 KiB-aligned buffer, 4 KiB-multiple
transfers, channel zero, and the AXI-MM address supplied to the workflow. Other
values are configurable functionality, not qualified tuples.

## Concurrency and boundedness

Slots move through free, owned, queued, running, complete, and reaping states.
Submitters claim slots with a bounded compare/exchange scan. Workers claim
queued slots independently and publish results with release ordering. Polling
reaps only physically completed slots. There is no spill queue, inline
fallback, per-submission thread, callback, condition-variable wait, or heap
allocation in steady-state `submit()`/`poll()`.

The backend may use more than one fixed worker and channel, but that does not
prove parallel DMA engines, throughput scaling, ordering, or absence of host
buffer races. Graph dependencies and application resource declarations remain
the ordering contract.

## Timeout, reset, and shutdown

The character-device transfer cannot be safely canceled by this ABI.
`cancel()` returns `UNSUPPORTED`.

If a queued request expires before a worker claims it, the worker publishes a
timeout without touching hardware. If a running request expires, the slot is
marked timed out but remains quarantined until `pread()`/`pwrite()` physically
returns. Only then does polling publish `TIMEOUT` and release the dependency.
This prevents buffer reuse while the kernel could still access the span. A
permanently hung driver can therefore prevent `step()` or shutdown from
returning; falsely completing the graph would violate memory lifetime.

Backend `reset()` is allowed only with no outstanding request. The Linux
adapter closes and reopens the configured endpoints. This is a soft file-handle
recovery attempt, not a PCIe function-level reset, driver reload, FPGA reset,
or bitstream reinitialization. Device loss cannot be repaired by this soft
reset; the host must stop, repair/rebind the deployment, and construct a new
backend.

Shutdown stops acceptance, drains queued/running work, joins all fixed workers,
and closes endpoints. A failed close leaves backend shutdown retryable. The
host must retain borrowed storage and deployment ownership until shutdown
succeeds or an external device/driver teardown becomes the final reclamation
boundary.

## Portable evidence

Automated CPU-only evidence covers:

- malformed configuration, payload, channel, access, range, and alignment
  rejection;
- H2C/C2H functional round trips through an injectable driver;
- bounded saturation with no spill or inline transfer;
- timeout quarantine until physical worker return;
- error, reset-required, health, soft reset, and shutdown behavior;
- concurrent submitters, fixed workers, and polling under ThreadSanitizer;
- zero allocation in steady-state submit/worker/poll;
- strict GCC, Clang, MSVC, AddressSanitizer, and UndefinedBehaviorSanitizer
  builds.

The opt-in qualification tool writes and reads deterministic patterns through
the real nodes, separates warm-up from measurement, checks every byte, and
emits raw per-direction:

- submit host-call duration;
- completion wait duration;
- accumulated poll host-call duration;
- poll count and completed byte count.

The workflow also records kernel, module, PCI, compiler, runtime, driver, and
bitstream identity. It uploads evidence without modifying the support matrix.

## Qualification exit gates

M10 can move from Candidate to Complete only after a declared tuple records:

- PCI vendor/device ID, BDF, link width/speed, IOMMU policy, NUMA placement,
  CPU, OS, kernel, PREEMPT_RT state, compiler, and runtime build;
- exact `dma_ip_drivers` revision/module build and module parameters;
- FPGA part, XDMA IP configuration, bitstream hash, clocks, channel mode, and
  memory-map contract;
- host alignment, page-lock policy, transfer sizes, queue depth, worker
  affinity/priority, warm-up, sample count, and thresholds selected before
  measurement;
- repeated H2C/C2H integrity and steady-state resource evidence;
- saturation, timeout, driver/device loss, reset, shutdown, and reboot/rebind
  recovery evidence;
- raw latency decomposition and an explicit reviewed pass/fail record.

No tuple has completed those gates in the 0.11.0 support matrix.

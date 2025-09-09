# IO and kernel-bypass options

The logging subsystem offers an asynchronous file sink tuned for stress and
worst-case durability testing.

## Disk logging

`Logger::AsyncRingFileSink` supports:

- Preallocating a target log file to avoid growth-related fragmentation.
- Optional `O_DIRECT|O_DSYNC` open flags via the `directSync` constructor
  parameter to bypass caches when benchmarking durable paths.
- A bounded queue that drops excess log records and tracks the drop count
  via `dropped()` so tests can assert on pressure.

## Network / IPC

The experimental `KernelBypassSocket` (`include/simcore/kernel_bypass.hpp`)
wraps platform specific kernel-bypass facilities:

- `io_uring` on Linux.
- I/O completion ports on Windows.

The `poll()` API allows DPDK-style busy polling for NIC-bound telemetry when
streaming live traces.

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

For network exporters `Logger::AsyncKernelBypassSink` pairs a bounded queue
with the `simcore::KernelBypassSocket` helper. The sink pushes log lines to a
socket using token-bucket backpressure identical to the file sink. On Linux the
socket helper can opt into io_uring submission by defining
`SIMCORE_ENABLE_IO_URING` at build time; otherwise it transparently falls back
to non-blocking `send()` calls so the same interface works across platforms.

## Code anchors

- File sink: `Logger::AsyncRingFileSink::write`, `Logger::AsyncRingFileSink::run`; `include/simcore/logger.hpp`
- Kernel-bypass sink: `Logger::AsyncKernelBypassSink::write`, `Logger::AsyncKernelBypassSink::run`; `include/simcore/logger.hpp`
- Kernel bypass helper: `simcore::KernelBypassSocket::submit`, `simcore::KernelBypassSocket::poll`; `include/simcore/kernel_bypass.hpp`


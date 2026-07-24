# Asynchronous I/O Experiments

The current logging sinks are service-thread experiments. They are not
allocation-free RT-lane sinks, and `io_uring` is not kernel bypass.

## File sink

`Logger::AsyncRingFileSink` can preallocate a file, use a bounded deque, count
drops, and request `O_DSYNC` plus `O_DIRECT` on supported Linux builds. Its
producer path still uses a mutex, copies/allocates strings, and notifies a
condition variable. `O_DIRECT` also imposes buffer alignment and size rules
that ordinary log strings may not satisfy; write return values are not
currently promoted into a complete error contract.

## Socket sink

`Logger::AsyncKernelBypassSink` uses a service thread and
`KernelBypassSocket`. With `SIMCORE_ENABLE_IO_URING` and liburing available, the
helper submits normal socket sends through io_uring. Otherwise it loops over
`send()` and depends on the descriptor's blocking mode.

The io_uring path owns pending payloads in `std::list<std::string>` and can
allocate. It remains a kernel-mediated socket API; no DPDK, AF_XDP, RDMA, or
other kernel-bypass transport is implemented. The current class name is legacy
and remains misleading.

## Target

M6 target-runtime lanes publish fixed-size records into preallocated atomic
slots with schema-visible overwrite/drop behavior. The library intentionally
does not create a telemetry transport thread. A future adapter may own
formatting, file/network I/O, retries, and shutdown on a non-RT service lane;
the legacy logger sinks above do not inherit the M6 contract.

## Code anchors

- File/socket sinks: `include/simcore/logger.hpp`
- io_uring/socket helper: `include/simcore/kernel_bypass.hpp`
- Observability target: [observability](observability.md)

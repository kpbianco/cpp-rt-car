# Observability

The runtime collects execution events with `bintrace`.  These snapshots can be
exported into several formats:

## Chrome trace
Use `trace_export::write_chrome_trace` to produce a JSON file consumable by the
Chrome trace viewer.

## ETW CSV adapter
`trace_export::write_etw_trace` emits comma separated values with columns
`tsc,thread,code,a,b`.  The output can be imported into ETW tooling or any CSV
processor.

## eBPF text adapter
`trace_export::write_ebpf_trace` writes each event on its own line using a
space separated format: `tsc thread code a b`.  This is suitable for simple
processing by eBPF-based pipelines.

## Queue instrumentation

The worker pool's bounded MPMC ring emits `queue_push` and `queue_pop` trace
events whenever a job is enqueued or dequeued. These samples include the queue
depth (`a`) and capacity (`b`), allowing dashboards to visualise saturation and
alert when the buffer is routinely full.

## Worker pool counters

Emergency launches increment the `worker.emergency_spawns` counter. The value is
reported via the metrics registry and mirrors the number of detached helper
threads created by the emergency path, making it straightforward to detect when
overload handling is triggered.

## Metrics JSON snapshots

The runtime can export metrics snapshots as JSON. Two modes are available for
how rolling histograms and resettable counters are treated between emissions:

| Mode                      | Histogram semantics                                   | Reset behaviour                                             | Typical use            |
|---------------------------|--------------------------------------------------------|--------------------------------------------------------------|------------------------|
| `--metrics-json`          | Percentiles (`p50/p95/p99`) accumulate for the entire run. | No reset; counters and histograms remain cumulative totals. | CI baselines or long runs |
| `--metrics-json-interval` | Percentiles cover only the samples collected since the last emission. | Rolling histograms and resettable counters are cleared after the snapshot is written. | Autotuners and live sweeps |

## Log drop accounting

The `log_drops` counter is the sum of two low-level metrics: `logger.dropped`
records how many log lines the async logger could not enqueue, while
`trace.dropped` tracks trace ring buffer losses. Keeping the composite counter
ensures alerting stays robust even if individual sources fluctuate.

## Code anchors

- Trace capture: `bintrace::Trace::log`, `bintrace::Trace::snapshot`; `include/simcore/bintrace.hpp`
- Chrome trace export: `trace_export::write_chrome_trace`; `tools/trace_export.hpp`
- ETW CSV export: `trace_export::write_etw_trace`; `tools/trace_export.hpp`
- eBPF text export: `trace_export::write_ebpf_trace`; `tools/trace_export.hpp`
- Queue events: `bintrace::log_queue_push`, `bintrace::log_queue_pop`; `include/simcore/bintrace.hpp`
- Ring buffer hooks: `BoundedMPMCQueue::try_push`, `BoundedMPMCQueue::try_pop`; `include/simcore/job_queue.hpp`
- Emergency counter: `WorkerPool::handleEmergency`; `include/simcore/worker_pool.hpp`
- Log drop aggregation: `SimCore::setMetrics`, `SimCore::publishCounters`; `include/simcore/SimCore.hpp`


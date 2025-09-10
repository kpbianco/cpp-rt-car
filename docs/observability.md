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

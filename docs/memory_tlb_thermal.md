# Memory/TLB/Thermal Discipline

## TLB Strategy

- Use 2 MB huge pages for hot arrays to minimise TLB pressure.
- Apply page coloring or `madvise` hints for streaming buffers.
- Tune software prefetch distances per simulation phase and disable them when they become harmful.

## Thermal Management

- Monitor on-die telemetry through RAPL/HWP interfaces.
- Add a "thermal limp" rung to the degrade ladder by reducing substeps before hardware throttling would cause larger stalls.

These guidelines complement the existing real-time hardening steps and help maintain predictable performance under load.

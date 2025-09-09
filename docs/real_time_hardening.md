# Real-time Hardening

This project includes optional scripts that attempt to tune the operating system for
lower and more deterministic latency.  These scripts are invoked by CI and can be run
locally as well.  They are **best-effort** and will silently skip steps when
permissions or features are unavailable.

## Linux

`tools/rt_harden.sh` performs the following actions when possible:

- allows unlimited locked memory so calls to `mlockall` succeed
- disables transparent huge pages
- sets `kernel.perf_event_paranoid=3` to guard against unwanted perf events
- masks the `irqbalance` service so interrupts may be pinned manually
- requests the `performance` CPU governor and disables deep C-states
- pins the invoking shell to CPU 0 as a lightweight substitute for `isolcpus`

Many hardening features require boot parameters (e.g. `isolcpus`, `nohz_full`,
`rcu_nocbs`) or elevated permissions.  Configure these through your bootloader
when building a real-time system.  Running `tools/rt_harden.sh` again with root
rights after boot allows you to set IRQ affinity and SCHED_FIFO/SCHED_DEADLINE
policies for your application.  To undo the power settings use the system's
standard power management tools.

## Windows

`tools/rt_harden.ps1` applies a comparable set of tweaks on Windows runners:

- selects the active power plan and forces 100% minimum/maximum processor state
- disables processor idle states to minimize core parking latency
- requests an MMCSS class (`Pro Audio`) for the current process
- calls `timeBeginPeriod(1)` to mitigate timer coalescing on recent Windows
  versions

Additional items such as HPET vs. TSC selection, timeBeginPeriod policy on
Windows 11 and QPC drift detection require administrative access or dedicated
hardware support and cannot be reliably configured inside CI.  Refer to the
Windows documentation for enabling those features manually.

## Power and C-States

Both scripts attempt to pin the processor at the highest P-state and disable
deep C-states.  This behaviour can be toggled at runtime by re-running the
scripts: invoking them applies the low-latency configuration, while restoring a
balanced power profile can be done with native tools such as `cpupower` on Linux
or `powercfg` on Windows.

See also [Memory/TLB/Thermal Discipline](memory_tlb_thermal.md) for in-application strategies.

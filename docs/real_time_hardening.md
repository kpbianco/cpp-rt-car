# Real-Time Host Experiments

The scripts in this repository are privileged, best-effort host mutation
helpers. They are not part of the RTFW library, do not establish RT
correctness, and do not produce an RT2 qualification.

Read each script before running it on a machine you care about. Several changes
are system-wide and the scripts do not provide a complete rollback.

## Linux helper

`tools/rt_harden.sh` attempts to:

- raise the invoking shell's locked-memory limit;
- disable transparent huge pages;
- set `kernel.perf_event_paranoid=3`;
- mask and stop `irqbalance`;
- select the performance governor and request a shallow package idle policy
  when `cpupower` is available;
- pin the script process to CPU 0.

Failures are logged and generally tolerated. Pinning the script process is not
CPU isolation, and it does not arrange application affinity, IRQ routing, or
PREEMPT_RT scheduling. The helper does not validate boot parameters, kernel
configuration, firmware, PCIe topology, driver behavior, or workload deadlines.

## Windows helper

`tools/rt_harden.ps1` attempts to modify the active power scheme, call an MMCSS
API for the PowerShell process, and request a 1 ms multimedia timer period.
These actions do not apply a documented policy to future RTFW worker threads
and do not establish bounded wake-up latency.

## CI use

Some CI jobs call these helpers before functional tests. Shared virtual CI
runners are unsuitable for latency qualification, and a successful helper call
does not strengthen the meaning of those tests. The jobs remain portability and
regression evidence only.

## Qualification path

A deployment pursuing RT2 must use a dedicated, versioned host recipe with
preflight checks that fail closed. The required hardware/kernel/driver/workload
record and measurements are listed in the
[product contract](product_contract.md). Host setup and library behavior stay
separate.

Release 0.7 retains an optional read-only strict Linux prerequisite preflight
for the target `rt::Runtime`. It checks clock support, PREEMPT_RT, memory-lock
coverage, isolated affinity, and realtime scheduling before runtime threads
start. It deliberately does not apply any of those settings, and passing it is
not an RT2 qualification. See the
[time/platform contract](time_platform.md).

## Code anchors

- Linux experiment: `tools/rt_harden.sh`
- Windows experiment: `tools/rt_harden.ps1`
- Qualification requirements: [product contract](product_contract.md)
- Runtime preflight: [time/platform contract](time_platform.md)

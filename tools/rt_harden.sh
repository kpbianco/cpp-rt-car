#!/usr/bin/env bash
# Apply best-effort real-time settings for CI environments.
set -euo pipefail

log() { echo "[rt_harden] $*"; }

SUDO=""
if command -v sudo >/dev/null; then
  SUDO="sudo"
fi

# Ensure memory locking is allowed
ulimit -l unlimited || log "mlockall may fail (unable to set unlimited lockable memory)"

# Disable transparent huge pages
if [ -f /sys/kernel/mm/transparent_hugepage/enabled ]; then
  $SUDO sh -c 'echo never > /sys/kernel/mm/transparent_hugepage/enabled' 2>/dev/null \
    && log "transparent hugepages disabled" || log "unable to disable transparent hugepages"
fi

# Guard perf events
$SUDO sysctl kernel.perf_event_paranoid=3 >/dev/null 2>&1 \
  && log "perf_event_paranoid set to 3" || log "unable to set perf_event_paranoid"

# Stop irqbalance to allow manual IRQ affinity
if command -v systemctl >/dev/null; then
  $SUDO systemctl mask --now irqbalance.service >/dev/null 2>&1 \
    && log "irqbalance service masked" || log "unable to mask irqbalance"
fi

# Attempt to set CPU governor to performance and disable deep C-states
if command -v cpupower >/dev/null; then
  $SUDO cpupower frequency-set -g performance >/dev/null 2>&1 \
    && log "CPU frequency governor set to performance" || log "unable to set performance governor"
  $SUDO cpupower set -b 0 >/dev/null 2>&1 \
    && log "package C-state limit disabled" || log "unable to disable C-states"
fi

# Use taskset to pin current shell to first CPU as a simple isolcpus substitute
if command -v taskset >/dev/null; then
  taskset -p 0x1 $$ >/dev/null 2>&1 && log "pinned current process to CPU0" || log "unable to pin CPU affinity"
fi

log "real-time hardening setup complete"

# cpp-rt-car

A compact real-time runtime sample showcasing the scheduler, worker pool and
metrics registry used across the project.

## Metrics

Use the metrics registry exports to capture latency percentiles and counters
from a run. The cumulative export keeps rolling histograms intact for the entire
lifetime of the process, while the interval export emits per-window samples.

```bash
# Capture a full-run snapshot with cumulative percentiles
./rt-car --metrics-json > metrics-baseline.json

# Continuously sweep for per-window statistics (p50/p95/p99 reset each loop)
while sleep 1; do
  ./rt-car --metrics-json-interval;
done
```

The interval mode resets the rolling histograms and counters that support
interval semantics after each snapshot, letting live dashboards or autotuners
observe how tweaks affect the most recent window without restarting the
application.

# Checked Runtime Profiles

These complete RTFW 1.1+ profiles are functional examples:

- `default.json`: two-worker bounded-throughput executor;
- `default_fast.json`: four-worker bounded-throughput executor;
- `default_safe.json`: one-worker static executor used by CI.

The names describe example intent, not measured performance or safety.
Deployment configurations require workload- and host-specific validation.
See [runtime profiles](../docs/runtime_profiles.md).

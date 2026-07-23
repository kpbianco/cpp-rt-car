# Real-Time Readiness Gates

This checklist is a release/qualification gate, not a feature inventory. RTFW
0.4 has not completed any end-to-end RT2 qualification.

## Portable runtime gates

- [ ] Configuration is finalized into an immutable graph and bounded resource
  plan.
- [x] Invalid/foreign handles, cyclic graphs, and declared resource conflicts
  fail before start (M2 functional gate; omitted declarations remain host
  error).
- [ ] The selected RT lane creates no hidden threads and performs no heap
  allocation, file I/O, or blocking lock after start.
- [ ] Every queue-full, arena-overflow, timeout, cancellation, and shutdown path
  has explicit bounded behavior and tests.
- [x] The M3 CPU executor's queue saturation is bounded and creates no
  emergency or detached helper (functional gate; complete frame overload policy
  remains M4).
- [ ] Host-driven steps never sleep; self-paced operation uses absolute release
  times.
- [ ] Multiple runtime instances have isolated clocks, numerical policy,
  allocator state, trace state, and device state.
- [ ] Trace/metrics records use fixed storage and a versioned schema.
- [ ] Snapshot input is bounds-checked, versioned, fuzzed, and tied to graph and
  configuration identity.
- [x] The C and C++ embedding APIs can register and execute real host work with
  typed lifecycle/graph/executor errors (M1–M3 functional gate; not an RT
  qualification).

## Deployment qualification gates

- [ ] A named hardware, firmware, kernel, driver, toolchain, configuration, and
  workload tuple is frozen.
- [ ] CPU isolation, IRQ placement, affinity, scheduling, memory locking, power
  policy, and device topology pass a fail-closed preflight.
- [ ] Release, wake-up, compute, device-completion, slack, and miss samples are
  captured for a predeclared duration.
- [ ] Pass/fail thresholds are selected before measurement and raw artifacts
  are retained.
- [ ] Overload, thermal, device loss/reset, and shutdown behavior pass on the
  target.
- [ ] The qualification record is reproducible from a clean checkout.

See [the product contract](product_contract.md) for RT tiers and
[the roadmap](roadmap.md) for implementation ownership.

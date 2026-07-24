# Real-Time Readiness Gates

This checklist is a release/qualification gate, not a feature inventory. RTFW
0.5 has not completed any end-to-end RT2 qualification.

## Portable runtime gates

- [x] Target `rt::Runtime` configuration is finalized into an immutable graph
  and bounded CPU resource plan (M4 functional gate).
- [x] Invalid/foreign handles, cyclic graphs, and declared resource conflicts
  fail before start (M2 functional gate; omitted declarations remain host
  error).
- [x] The target CPU runtime lane creates no hidden threads and performs no
  intentional heap allocation, file I/O, condition-variable wait, or blocking
  mutex after start (M4 instrumented functional gate; user callbacks and
  legacy/device paths remain outside it).
- [ ] Every queue-full, arena-overflow, timeout, cancellation, and shutdown path
  has explicit bounded behavior and tests.
- [x] Queue and task-scratch saturation are bounded, expose
  `reject_submission` / `fail_frame`, and create no emergency or detached
  helper (M4 functional gate).
- [ ] Host-driven steps never sleep; self-paced operation uses absolute release
  times.
- [ ] Multiple runtime instances have isolated clocks, numerical policy,
  allocator state, trace state, and device state.
- [ ] Trace/metrics records use fixed storage and a versioned schema.
- [ ] Snapshot input is bounds-checked, versioned, fuzzed, and tied to graph and
  configuration identity.
- [x] The C and C++ embedding APIs can register and execute real host work with
  typed lifecycle/graph/executor/memory errors (M1–M4 functional gate; not an RT
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

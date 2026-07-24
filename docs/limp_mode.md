# Degradation and Limp-Mode Experiment

`SimCore` contains an experimental four-rung degradation mechanism. Depending
on budget/governor/watchdog state, it can disable visualizers, reduce substeps,
and mark broadphase work as coarse. Setting `Settings::limpMode` disables the
budget monitor and binary trace during settings application and selects the
highest rung.

This is policy scaffolding, not a general overload-safety guarantee:

- the runtime cannot know whether a host callback observes the flags;
- rungs are one-way for the runtime lifetime;
- callbacks and state transitions are not described by a stable ABI;
- the watchdog owns another thread and uses mutex/condition-variable state;
- a watchdog callback mutates runtime degradation state concurrently;
- there is no qualified proof that the reduced workload meets a deadline.

Release 0.6 adds a separate M5 target-path signal:
`CallbackContext::degradation_level`. A one-shot watchdog event is consumed and
the capped level is incremented only by the frame thread after the graph
returns. The watchdog service lane never calls the host or mutates that state.
Callbacks must explicitly choose cheaper work; 0.7 does not automatically
disable phases and has no recovery policy. See the
[time/platform contract](time_platform.md).

Automatic optional-work shedding as a compiled-graph policy, recovery
semantics, and versioned telemetry remain future work.

## Code anchors

- Settings and rung behavior: `SimCore::Settings`,
  `SimCore::applyDegradeRung`; `include/simcore/SimCore.hpp`
- Watchdog experiment: `rt::Watchdog`; `rt/include/rt/watchdog.hpp`
- Target-path watchdog/degradation:
  `rt/src/watchdog_monitor.cpp`, `rt/src/host_runtime.cpp`

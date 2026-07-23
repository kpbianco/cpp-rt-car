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

The target lifecycle will make optional-work shedding an explicit compiled-graph
policy with thread-safe state transitions, telemetry, and overload tests.

## Code anchors

- Settings and rung behavior: `SimCore::Settings`,
  `SimCore::applyDegradeRung`; `include/simcore/SimCore.hpp`
- Watchdog experiment: `rt::Watchdog`; `rt/include/rt/watchdog.hpp`

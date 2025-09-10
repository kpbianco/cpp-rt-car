# Scheduler

This module provides a simple work-stealing scheduler used for experiments with
real‑time task management. The scheduler supports two policies:

* **EDF (Earliest Deadline First)** – tasks are ordered by their deadline and
  run accordingly. This is useful for multi‑rate subsystems such as fast feedback
  loops and sensor processing.
* **Fixed‑Frame** – tasks are sorted by their period, emulating a fixed frame
  rate scheduler where shorter period tasks run first.

Each worker thread owns a local queue backed by a thread‑local memory arena to
reduce remote NUMA traffic. Tasks have priorities and an aging counter; every
time a task waits in the queue its age increases, effectively boosting its
priority so that low priority tasks will eventually run and avoid starvation.

The scheduler exposes a helper `pop_next_with_aging` function that selects the
next task based on the `(priority + age)` heuristic. This function is used by
unit tests to verify the aging behaviour.


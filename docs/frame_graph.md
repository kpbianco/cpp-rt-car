# GPU Frame Graph Stub

This repository includes a lightweight frame graph interface intended for
experiments with GPU/accelerator scheduling.  The implementation lives in
[`gpu/frame_graph.hpp`](../gpu/frame_graph.hpp) and provides:

* **Resource lifetime tracking** – resources are released after their last use.
* **Timeline semaphores** – CPU and GPU progress are exposed via atomic
  counters that can be waited on.
* **Zero‑copy pinned staging** – resources allocate pinned memory through the
  existing HAL allocator.
* **Async overlap budgets** – execution records CPU and GPU durations and
  computes the amount of overlap between them.
* **Mixed compute stubs** – helper functions allow submitting placeholder
  SPIR‑V or CUDA kernels without introducing new plumbing.

The frame graph is a stub and intentionally minimal; it is meant to be
replaced by a real implementation once a concrete GPU backend is available.

## Code anchors

- Resource lifetime tracking: `FrameGraph::create_resource`, `FrameGraph::free_dead_resources`; `gpu/frame_graph.hpp`
- Timeline semaphores: `TimelineSemaphore::wait`, `TimelineSemaphore::signal`; `gpu/frame_graph.hpp`
- Async overlap budget: `FrameGraph::execute`; `gpu/frame_graph.hpp`
- Mixed compute stubs: `FrameGraph::add_pass`; `gpu/frame_graph.hpp`


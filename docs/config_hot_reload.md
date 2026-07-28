# Config Hot-Reload Experiment

`core::ConfigHotReloader` is a standalone test utility for a two-integer
`core::Config`:

```text
<major> <minor>
```

It polls file modification time, parses with `std::ifstream`, allocates a new
`shared_ptr`, and atomically publishes that pointer when the major version
matches. It is not connected to `SimCore::Settings`, the demo, plugins, or the
M13 runtime profile schema.

Filesystem access, parsing, and allocation make it unsuitable for an RT lane.
The target `rt::Runtime` accepts `RuntimeConfig` or 25 strict key/value fields
during its configuring state, then freezes configuration at finalization.
M13 adds a separate bounded, allocation-free, transactional parser for a
complete JSON profile in `<rt/profile.hpp>`. The caller still performs file I/O
off-lane and applies the result only while configuring; neither surface watches
files or hot-swaps a finalized runtime.

A future control-plane update may build an immutable configuration off-lane and
apply an explicitly supported subset at a safe boundary.

## Code anchors

- Toy config type: `core/include/core/config.hpp`
- Reload helper: `core/include/core/config_hot_reload.hpp`
- Typed runtime configuration: `rt/include/rt/runtime.hpp`,
  `rt/src/host_runtime.cpp`
- Complete runtime profiles: `rt/include/rt/profile.hpp`,
  `rt/src/runtime_profile.cpp`
- Runtime lifecycle: [host runtime contract](host_runtime.md)

# Config Hot-Reload Experiment

`core::ConfigHotReloader` is a standalone test utility for a two-integer
`core::Config`:

```text
<major> <minor>
```

It polls file modification time, parses with `std::ifstream`, allocates a new
`shared_ptr`, and atomically publishes that pointer when the major version
matches. It is not connected to `SimCore::Settings`, the demo, plugins, or the
autotune profile schema.

Filesystem access, parsing, and allocation make it unsuitable for an RT lane.
The target runtime instead parses and validates typed configuration during
configure/finalize. A future control-plane update may build an immutable
configuration off-lane and apply an explicitly supported subset at a safe
boundary.

## Code anchors

- Toy config type: `core/include/core/config.hpp`
- Reload helper: `core/include/core/config_hot_reload.hpp`
- Runtime lifecycle target: [product contract](product_contract.md)

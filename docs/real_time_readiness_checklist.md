# Real-Time Readiness Checklist

Use this checklist to assess whether the system is ready for real-time use.

- [ ] Deterministic execution paths validated by unit tests.
- [ ] All dynamic allocations are bounded or eliminated.
- [ ] Plugins can be hot-loaded and unloaded without restarting the runtime.
- [ ] Watchdog timers and monitoring enabled.
- [ ] Threat model reviewed and mitigations implemented.

## Code anchors

- Determinism validation: `TEST(SnapshotRollback, ReplayHashEquality)`; `tests/test_snapshot.cpp`
- Bounded allocations: `rt::FrameArena`, `rt::FrameArenaPool`; `include/simcore/rt_memory.hpp`
- Plugin lifecycle: `rt::PluginManager::load`, `rt::PluginManager::unload`; `rt/src/plugin_manager.cpp`
- Watchdog monitoring: `rt::Watchdog::arm`, `SimCore::executeFrame`; `rt/include/rt/watchdog.hpp`, `include/simcore/SimCore.hpp`
- Threat mitigations: `enable_sandbox`; `include/simcore/sandbox.hpp`


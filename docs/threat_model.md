# Threat Model

This document outlines potential threats to the runtime and mitigation
strategies.

- **Plugin misuse**: Untrusted plugins could attempt to exploit the process.
  Only load plugins from trusted sources and verify ABI versions.
- **Resource exhaustion**: Plugins may allocate excessive memory or CPU. Use
  runtime limits and monitoring to detect and contain abuses.
- **Denial of Service**: Malformed inputs may trigger unexpected states.
  Validate inputs and employ watchdog timers.

## Code anchors

- Plugin mitigations: `rt::PluginManager::load`, `rt::PluginManager::unload`; `rt/src/plugin_manager.cpp`, `rt/include/rt/plugin_api.h`
- Resource limits: `simcore::TokenBucket::try_acquire`; `include/simcore/backpressure.hpp`
- Watchdog guardrails: `rt::Watchdog::arm`, `rt::Watchdog::disarm`; `rt/include/rt/watchdog.hpp`


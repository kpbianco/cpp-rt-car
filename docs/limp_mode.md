# Limp Mode

The `SimCore::Settings` structure now provides a `limpMode` flag.
When enabled, SimCore aggressively trims non-essential features to
prioritize frame delivery under extreme load. Specifically it:

- disables the budget monitor and binary trace logging;
- forces the highest degradation rung, disabling visualizers,
  enabling coarse broadphase, and limiting substeps to one.

This switch acts as a global guardrail for deployments that must remain
responsive even when resources are severely constrained.

## Code anchors

- Settings flag: `SimCore::Settings::limpMode`; `include/simcore/SimCore.hpp`
- Limp-mode behaviour: `SimCore::applySettings`, `SimCore::applyDegradeRung`; `include/simcore/SimCore.hpp`


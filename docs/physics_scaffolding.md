# Physics Scaffolding

The physics module provides engine-agnostic building blocks:

- **Hamiltonian Integrators**: `stormerVerlet` offers a leapfrog option for energy-conserving subsystems.
- **Constraint Solver Depth**: `SolverSettings` exposes warm-start caches, split-impulse and shock-propagation vs. block Gauss-Seidel toggles. `IslandManager` supplies basic islanding and sleeping heuristics.
- **Narrowphase Shells**: `GJKEPANarrowPhase` and `MPRNarrowPhase` expose hooks for common convex collision detectors. `ContactCache` demonstrates temporal coherence and `ConservativeAdvancementCCD` sketches continuous collision detection.

These scaffolds are intentionally lightweight and serve as extension points for real engines.

## Code anchors

- Hamiltonian integrators: `simphys::stormerVerlet`; `include/simcore/physics/integrators.hpp`
- Constraint solver depth: `simphys::SolverSettings`, `simphys::IslandManager::update`; `include/simcore/physics/constraint_solver.hpp`
- Narrowphase shells: `simphys::GJKEPANarrowPhase::generateContacts`, `simphys::MPRNarrowPhase::generateContacts`; `include/simcore/physics/collision_pipeline.hpp`
- Temporal coherence & CCD: `simphys::ContactCache::contacts`, `simphys::ConservativeAdvancementCCD::sweep`; `include/simcore/physics/collision_pipeline.hpp`


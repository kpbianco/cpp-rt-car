# Physics Scaffolding

The physics module provides engine-agnostic building blocks:

- **Hamiltonian Integrators**: `stormerVerlet` offers a leapfrog option for energy-conserving subsystems.
- **Constraint Solver Depth**: `SolverSettings` exposes warm-start caches, split-impulse and shock-propagation vs. block Gauss-Seidel toggles. `IslandManager` supplies basic islanding and sleeping heuristics.
- **Narrowphase Shells**: `GJKEPANarrowPhase` and `MPRNarrowPhase` expose hooks for common convex collision detectors. `ContactCache` demonstrates temporal coherence and `ConservativeAdvancementCCD` sketches continuous collision detection.

These scaffolds are intentionally lightweight and serve as extension points for real engines.

# Minimal Samples

This directory contains tiny programs illustrating core simulation concepts.

- `particles_on_plane.cpp` – integrates a particle falling under gravity.
- `tyre_belt_on_drum.cpp` – scalar math kernel for belt tension around a drum.
- `gpu_offload_demo.cpp` – demonstrates OpenMP target offload.

Build a sample with `g++ -std=c++20 -I../include <file.cpp> -o <prog>`.
The GPU demo additionally requires OpenMP offload support (`-fopenmp`).

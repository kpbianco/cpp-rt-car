# Architecture Overview

## Phase DAG
```
[Input] -> [Simulation] -> [Output]
               |
         [Physics]
               |
        [Constraint]
```

The engine processes data in a directed acyclic graph (DAG). Each node
represents a phase; edges model dependencies and data flow.

## Memory Layout
```
struct Car {
    // Structure of Arrays (SoA)
    float pos_x[N];
    float pos_y[N];
    float vel_x[N];
    float vel_y[N];
}
```

Physics kernels operate on SoA buffers to maximise cache and SIMD
performance. Array dimensions align to cache lines for predictable access.

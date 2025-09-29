# Autotuning Summary

Generated from `results` using spec `tools/autotune/spec.yaml`.

## Hardware and environment

| Variable | Observed values |
| --- | --- |
| `env.cpu_model` | Intel(R) Xeon(R) Gold 6258R |
| `env.governor` | performance, powersave |
| `env.kernel` | 5.15.0-1051-azure |
| `env.os` | Linux |

## Budget and constraints

* **Executable**: `./build/bin/rtfw_demo`
* **Warmup / run**: 5s warmup, 20s sampled
* **Frame budget**: 5 ms
* **Default arguments**: `--rt`
* **Tiebreakers**: `p95_frame_ms / frame_budget_ms`, `stdev_frame_ms`, `queue_max`, `emergency_spawns`
* **Objective**: minimize `p99_frame_ms / frame_budget_ms`
* **Hard constraints:**
  * `missed_frames` == 0
  * `watchdog_trips` == 0
  * `log_drops` <= 0

## Best configuration

| Setting | Value |
| --- | --- |
| `objective` | 0.9421 |
| `threads` | physical |
| `chunk_target_us` | 140 |
| `aosoa_block` | 256 |
| `steal_threshold` | 3 |
| `prefetch_distance_bytes` | 1024 |
| `huge_pages` | yes |
| `governor_target_util` | 0.93 |
| `governor_hysteresis` | 0.02 |
| `fma_mode` | auto_no_when_deterministic |
| `emergency_spawn_enabled` | no |
| `arena_per_thread_mb` | 64 |
| `p99_frame_ms` | 4.71 |
| `p95_frame_ms` | 4.38 |
| `p50_frame_ms` | 3.89 |
| `stdev_frame_ms` | 0.42 |
| `queue_max` | 5 |
| `emergency_spawns` | 0 |
| `log_drops` | 0 |
| `missed_frames` | 0 |
| `watchdog_trips` | 0 |

## Pareto frontier (top 10)

| Rank | Objective | p99_frame_ms | queue_max | emergency_spawns | log_drops | Parameters |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 0.9421 | 4.71 | 5 | 0 | 0 | threads=physical; chunk_target_us=140; aosoa_block=256; steal_threshold=3; prefetch_distance_bytes=1024; huge_pages=yes; governor_target_util=0.93; governor_hysteresis=0.02; fma_mode=auto_no_when_deterministic; emergency_spawn_enabled=no; arena_per_thread_mb=64 |
| 2 | 0.9475 | 4.73 | 6 | 0 | 0 | threads=physical_plus_smt; chunk_target_us=150; aosoa_block=256; steal_threshold=2; prefetch_distance_bytes=512; huge_pages=yes; governor_target_util=0.94; governor_hysteresis=0.025; fma_mode=on; emergency_spawn_enabled=no; arena_per_thread_mb=48 |
| 3 | 0.9512 | 4.76 | 4 | 1 | 0 | threads=physical; chunk_target_us=130; aosoa_block=512; steal_threshold=4; prefetch_distance_bytes=1536; huge_pages=yes; governor_target_util=0.92; governor_hysteresis=0.02; fma_mode=auto_no_when_deterministic; emergency_spawn_enabled=yes; arena_per_thread_mb=80 |
| 4 | 0.9588 | 4.81 | 7 | 0 | 0 | threads=physical; chunk_target_us=160; aosoa_block=256; steal_threshold=5; prefetch_distance_bytes=2048; huge_pages=no; governor_target_util=0.9; governor_hysteresis=0.03; fma_mode=off; emergency_spawn_enabled=no; arena_per_thread_mb=56 |
| 5 | 0.9624 | 4.84 | 6 | 1 | 0 | threads=physical_plus_smt; chunk_target_us=120; aosoa_block=128; steal_threshold=1; prefetch_distance_bytes=768; huge_pages=yes; governor_target_util=0.95; governor_hysteresis=0.015; fma_mode=on; emergency_spawn_enabled=yes; arena_per_thread_mb=72 |

## How to use

```bash
./build/bin/rtfw_demo --config profiles/<cpu>-<os>.json --rt --metrics-json
```

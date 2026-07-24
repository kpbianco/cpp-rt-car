# DOE and Autotune Status

The repository contains a working design-of-experiments and analysis pipeline,
plus a synthetic application used to test that pipeline. It is not yet
integrated with the real `rtfw_demo`.

## What is verified

The synthetic workflow generates parameter mappings, runs candidates, evaluates
constraints/objectives, performs a small search, and writes profiles, JSONL
results, summaries, and reports.

Run its CI-sized smoke:

```bash
tools/autotune/run_smoke.sh
```

Run mapping/schema checks:

```bash
python3 tools/autotune/make_config.py --self-test
python3 tools/autotune/mapping_smoke.py
```

These commands validate tooling and schemas. They do not validate that
`rtfw_demo` consumes a generated profile.

## Why the default runtime spec is blocked

`tools/autotune/spec.yaml` describes the intended runtime integration. The
orchestrator launches an application with generated `--config` input,
`--run`-bounded warm-up/measurement windows, and optional `--rt` arguments.
RTFW 0.7's demo implements none of those three options and does not read
`RTFW_PROFILE`.

The current profile schema also contains settings that are not mapped into
`SimCore::Settings`, including scheduler-steal policy, huge pages, and AoSoA
layout selection. Consequently:

- do not run `spec.yaml` against `rtfw_demo` and interpret the result as a
  runtime tuning result;
- tracked files under `results/`, `profiles/`, and `reports/` are illustrative
  fixtures unless their provenance explicitly names another executable;
- generated recommendations are not production profiles.

M1 supplies the embedding runtime's typed configuration contract, M3 adds
executor policy, worker count, and queue capacity, M4 adds the memory and
overload fields, M5 adds direct finite cadence/deadline results, and M6 adds
cursor-based metric windows plus versioned provenance. The demo/profile mapper
still does not configure or consume the M1–M6 runtime surface, so runtime
autotuning remains invalid.

## Intended workflow after integration

After those milestones, a valid experiment will:

1. build a pinned Release artifact;
2. validate the target host and resolved runtime configuration;
3. warm up without contaminating the measurement interval;
4. collect direct end-to-end frame/deadline samples and required counters;
5. retain raw samples and environment provenance;
6. reject invalid or dropped-data trials;
7. validate finalists across seeds, scenarios, and repeated runs;
8. emit a profile tied to runtime schema and hardware identifiers.

The factor-to-config mapping and constraint language are documented in
[`tools/autotune/README.md`](../tools/autotune/README.md).

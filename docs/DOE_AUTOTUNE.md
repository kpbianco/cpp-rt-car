# DOE / Autotune Workflow

This guide expands on the quickstart snippet in the [README](../README.md#15-doe--autotune-workflow). It covers the pre-flight checks, describes the generated artifacts, and shows how to extend the default experiment design.

## 1. Pre-flight checklist

Before launching autotune runs, stabilise the platform so every replicate produces comparable interval metrics.

- **Thermals & frequency**: warm the system for a few minutes and pin clocks/turbo settings according to your reliability policy.
- **CPU isolation**: reserve cores for realtime work (cpuset, IRQ affinity, SMT policy) matching your production target.
- **Build artifacts**: ensure `./build/bin/rtfw_demo` exists — the orchestration script assumes a Release-equivalent binary is ready.

## 2. Run the full workflow

```bash
python3 tools/autotune/run_experiments.py \
  --spec tools/autotune/spec.yaml \
  --screen 32 \
  --replicates 3 \
  --local-iters 40 \
  --topk 5
```

### Flags explained

- `--spec` — DOE description (application entry point, metrics, factor space, objectives).
- `--screen` — number of Sobol/randomised candidates to evaluate before local search kicks in.
- `--replicates` — interval metric samples per evaluation; >1 smooths jittery hosts.
- `--local-iters` — bounded coordinate/local search iterations seeded from the best screening candidate.
- `--topk` — number of unique candidates to send through robustness validation.
- `--warmup-sec` — optional override for the warmup duration defined in the spec (seconds).
- `--run-sec` — optional override for the metrics sampling window (seconds).

> **Interval metrics only.** The runner always calls `rtfw_demo` with `--metrics-json-interval`. Cumulative stats (`--metrics-json`) are intentionally ignored because they hide short-lived regressions and make comparisons between runs ambiguous.

## 3. Generated artifacts

The workflow writes three directories at the repository root:

| Directory | Purpose | Key files |
| --- | --- | --- |
| `profiles/` | Drop-in machine profiles named `<cpu>-<os>.json`. The runtime auto-loads these alongside stock `default_safe`/`default_fast`. | `profiles/x86_64-Linux.json` (example) |
| `results/` | Raw experiment log and orchestration breadcrumbs suitable for CI diffing or ad-hoc plots. | `experiments.jsonl`, `summary.json`, `top_candidates.json`, `validation_summary.json` |
| `reports/` | Aggregated analytics from `tools/autotune/analyze.py` — Pareto sets, CSV summaries, best run report. | `pareto.json`, `summary.csv`, `best.json` |

All JSON files use UTF-8 + newline for easy `jq`/Python ingestion.

## 4. Tweaking the design space

The YAML spec drives every stage. To change what the tuner explores:

1. Edit [`tools/autotune/spec.yaml`](../tools/autotune/spec.yaml).
2. Adjust the `params` section (categorical/int/float/bool) with new ranges.
3. Expand `metrics.hard_constraints` or the objective expression to enforce new SLAs.
4. Re-run the command above — the script will resume from `results/experiments.jsonl` when possible.

For one-off sweeps or custom analytics, inspect `results/experiments.jsonl` with `jq` or re-run `tools/autotune/analyze.py` against the saved log.

## 5. Consuming the profile

After the run finishes, copy the generated profile into your deployment target or point `RTFW_PROFILE` at the emitted JSON. The runtime loads this alongside existing configs:

```bash
RTFW_PROFILE=$(uname -m)-$(uname -s) ./build/bin/rtfw_demo --rt --metrics-json
```

Because the profile was derived from interval metrics, keep monitoring both cumulative and interval outputs in production to ensure no long-horizon drift.

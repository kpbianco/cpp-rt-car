# Continuous autopilot

From the repository root, with `portfolio-control` cloned beside this repository:

```bash
./scripts/product-autopilot.sh
```

The command is resumable. It selects or generates one dependency-ready canonical
batch, validates and merges the control-plane contract, activates it here,
implements the complete batch through Codex, runs the full local verification,
performs a final adversarial audit, opens the target PR, waits for GitHub CI,
performs bounded causal repairs, merges, and repeats.

It may skip a hardware-blocked M18 qualification node and continue a dependency-
independent M19 software node. It cannot mark M18 complete, promote support
matrices, publish releases, sign artifacts, deploy, or claim CUDA/XDMA/RT1/RT2
without exact retained evidence.

State and logs live under the sibling control repository at
`.autopilot/cpp-rt-car/`. Rerun the same command after a machine interruption,
authentication outage, or repairable external failure. Use `--reset-state` only
after manually reconciling saved branches and PRs.

Useful bounded run:

```bash
./scripts/product-autopilot.sh --max-batches 1
```

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

python3 tools/autotune/run_experiments.py \
  --spec tools/autotune/spec_smoke.yaml \
  --screen 4 \
  --replicates 1 \
  --local-iters 1 \
  --topk 2

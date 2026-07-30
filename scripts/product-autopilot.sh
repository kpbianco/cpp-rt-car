#!/usr/bin/env bash
set -Eeuo pipefail

TARGET_ROOT="$(git rev-parse --show-toplevel)"
WORKSPACE="$(dirname "$TARGET_ROOT")"
CONTROL_REPO="${CONTROL_REPO:-$WORKSPACE/portfolio-control}"

[[ -x "$CONTROL_REPO/scripts/product-autopilot.sh" ]] || {
  echo "missing executable control-plane autopilot: $CONTROL_REPO/scripts/product-autopilot.sh" >&2
  exit 2
}

# This invocation is the explicit one-time authorization to merge only the
# scoped PRs created by the current resumable product run after all gates pass.
exec "$CONTROL_REPO/scripts/product-autopilot.sh" cpp-rt-car --auto-merge "$@"

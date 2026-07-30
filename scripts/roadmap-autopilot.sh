#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(git rev-parse --show-toplevel)"
CONTROL_REPO="${CONTROL_REPO:-$(realpath "$ROOT/../portfolio-control")}"
[[ -x "$CONTROL_REPO/scripts/run-product-roadmap.sh" ]] || {
  echo "missing portfolio-control autopilot: $CONTROL_REPO/scripts/run-product-roadmap.sh" >&2
  exit 2
}
export AUTOPILOT_MODE="${AUTOPILOT_MODE:-merge-software}"
exec "$CONTROL_REPO/scripts/run-product-roadmap.sh" cpp-rt-car "$@"

#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(git rev-parse --show-toplevel)"
COMMANDS="$ROOT/contracts/verification.commands"
LOG_DIR="$ROOT/docs/evidence/local"
MODE="${1:-full}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="$LOG_DIR/verify-${MODE}-${STAMP}.log"

case "$MODE" in
  contract) MODE_RANK=1 ;;
  quick) MODE_RANK=2 ;;
  full) MODE_RANK=3 ;;
  *)
    printf 'usage: %s [contract|quick|full]\n' "$0" >&2
    exit 2
    ;;
esac

[[ -f "$COMMANDS" ]] || {
  printf 'missing verification command file: %s\n' "$COMMANDS" >&2
  exit 2
}

mkdir -p "$LOG_DIR"
exec > >(tee -a "$LOG") 2>&1

printf 'repository=%s\n' "$ROOT"
printf 'mode=%s\n' "$MODE"
printf 'started=%s\n' "$(date -u --iso-8601=seconds)"
printf 'head=%s\n' "$(git -C "$ROOT" rev-parse HEAD)"

SECTION="contract"
while IFS= read -r line || [[ -n "$line" ]]; do
  if [[ "$line" =~ ^#[[:space:]]*scope:([a-z]+)[[:space:]]*$ ]]; then
    SECTION="${BASH_REMATCH[1]}"
    continue
  fi
  [[ -z "${line//[[:space:]]/}" ]] && continue
  [[ "$line" =~ ^[[:space:]]*# ]] && continue

  case "$SECTION" in
    contract) SECTION_RANK=1 ;;
    quick) SECTION_RANK=2 ;;
    full) SECTION_RANK=3 ;;
    *)
      printf 'unknown verification scope %q in %s\n' "$SECTION" "$COMMANDS" >&2
      exit 2
      ;;
  esac

  if (( SECTION_RANK <= MODE_RANK )); then
    printf '\n>>> [%s] %s\n' "$SECTION" "$line"
    bash -lc "cd \"$ROOT\" && $line"
  fi
done < "$COMMANDS"

printf '\ncompleted=%s\n' "$(date -u --iso-8601=seconds)"
printf 'log=%s\n' "$LOG"

#!/usr/bin/env bash
# Single-TC head-to-head at 200 ms × 20 games. Like test.sh but
# narrowed to one movetime, no .dat file (results stay on console).
# Useful for quick A/B checks without waiting through all four TCs.
#
# Usage:
#   source <venv>/bin/activate    # python-chess required
#   ./test200.sh <engine1-cmd> <engine2-cmd>
#
# Examples:
#   ./test200.sh ./build/release/chesserazade ./build/release/chesserazade_old
#   ./test200.sh "./eng1 --opt" "./eng2 --opt"
#
# Env-var overrides (optional):
#   GAMES=40             (default 20)
#   TIMES="100 500"      (default "200")
#   JOBS=-j8             (default -j   = phys cores - 1)
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <engine1-cmd> <engine2-cmd>" >&2
    exit 1
fi
ENG1="$1"
ENG2="$2"

GAMES="${GAMES:-20}"
TIMES="${TIMES:-200}"
JOBS="${JOBS:--j}"

mkdir -p runs

for MS in $TIMES; do
    echo "=== movetime ${MS} ms ==="
    time python tools/match.py \
        --engine1 "$ENG1" \
        --engine2 "$ENG2" \
        --games "$GAMES" \
        --movetime "$MS" \
        $JOBS
done

#!/usr/bin/env bash
# Single-TC head-to-head at 1000 ms × 20 games. Like test.sh but
# narrowed to one movetime, no .dat file (results stay on console).
# Useful for the longest TC alone — that's where small bugs in
# search show up most clearly.
#
# Usage:
#   source <venv>/bin/activate    # python-chess required
#   ./test1000.sh <engine1-cmd> <engine2-cmd>
#
# Examples:
#   ./test1000.sh ./build/release/chesserazade ./build/release/chesserazade_old
#   ./test1000.sh "./eng1 --opt" "./eng2 --opt"
#
# Env-var overrides (optional):
#   GAMES=40             (default 20)
#   TIMES="500 2000"     (default "1000")
#   JOBS=-j8             (default -j   = phys cores - 1)
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <engine1-cmd> <engine2-cmd>" >&2
    exit 1
fi
ENG1="$1"
ENG2="$2"

GAMES="${GAMES:-20}"
TIMES="${TIMES:-1000}"
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

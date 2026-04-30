#!/usr/bin/env bash
# Time-control sweep — head-to-head between two engine builds at
# 100 / 200 / 500 / 1000 ms × 20 games each. Outputs:
#   wynik<MS>.dat        — match.py summary on stdout
#
# Usage:
#   source <venv>/bin/activate    # python-chess required
#   ./test.sh <engine1-cmd> <engine2-cmd>
#
# Examples:
#   ./test.sh ./build/release/chesserazade ./build/release/chesserazade_old
#   ./test.sh "./eng1 --opt" "./eng2 --opt"       # if you need to pass args
#
# Env-var overrides (optional):
#   GAMES=20            (default 20)
#   TIMES="100 500"     (default "100 200 500 1000")
#   JOBS=-j8            (default -j   = phys cores - 1)
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <engine1-cmd> <engine2-cmd>" >&2
    exit 1
fi
ENG1="$1"
ENG2="$2"

GAMES="${GAMES:-20}"
TIMES="${TIMES:-100 200 500 1000}"
JOBS="${JOBS:--j}"

mkdir -p runs

for MS in $TIMES; do
    echo "=== movetime ${MS} ms ==="
    time python tools/match.py \
        --engine1 "$ENG1" \
        --engine2 "$ENG2" \
        --games "$GAMES" \
        --movetime "$MS" \
        $JOBS \
        > "wynik${MS}.dat"
    echo "po ${MS}"
done

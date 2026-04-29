#!/usr/bin/env bash
# Time-control sweep — head-to-head between two engine builds at
# 100 / 200 / 500 / 1000 ms × 40 games each. Outputs:
#   wynik<MS>.dat        — match.py summary on stdout
#
# Usage:
#   source <venv>/bin/activate    # python-chess required
#   ./test.sh <engine1-cmd> <engine2-cmd>
#
# Examples:
#   ./test.sh ./build/release/chesserazade ./build/release/chesserazade_old
#   ./test.sh "./eng1 uci" "./eng2 uci"           # if you need to pass args
#
# Env-var overrides (optional):
#   GAMES=20            (default 40)
#   TIMES="100 500"     (default "100 200 500 1000")
#   JOBS=-j8            (default -j   = use all physical cores)
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <engine1-cmd> <engine2-cmd>" >&2
    exit 1
fi
ENG1="$1"
ENG2="$2"

GAMES="${GAMES:-40}"
TIMES="${TIMES:-100 200 500 1000}"
JOBS="${JOBS:--j}"

# `uci` is appended only if the user passed a bare path; preserves
# `./eng arg1 arg2` style commands intact.
if [[ "$ENG1" != *" "* ]]; then ENG1="$ENG1 uci"; fi
if [[ "$ENG2" != *" "* ]]; then ENG2="$ENG2 uci"; fi

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

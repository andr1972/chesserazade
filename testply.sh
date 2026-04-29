#!/usr/bin/env bash
# Ply-cap sweep — head-to-head between two engine builds at fixed
# movetime, varying the per-move depth cap. Each ply level plays
# 40 games, with a 200-fullmove draw cap to keep marathon endgames
# from stalling the run. Output:
#   wynikply<PLY>.dat    — match.py summary on stdout
#
# Usage:
#   source <venv>/bin/activate    # python-chess required
#   ./testply.sh <engine1-cmd> <engine2-cmd>
#
# Examples:
#   ./testply.sh ./build/release-optim/chesserazade ./build/release/chesserazade
#   ./testply.sh "./eng1 uci" "./eng2 uci"
#
# Env-var overrides (optional):
#   GAMES=20            (default 40)
#   TIME=2000           movetime in ms (default 500)
#   PLIES="8 12 16"     (default "10 12 14 16")
#   JOBS=-j8            (default -j   = use all physical cores)
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <engine1-cmd> <engine2-cmd>" >&2
    exit 1
fi
ENG1="$1"
ENG2="$2"

GAMES="${GAMES:-40}"
TIME="${TIME:-1000}"
PLIES="${PLIES:-1 2 3 4 5 6 7 8 9 11 13 15}"
JOBS="${JOBS:--j}"

# `uci` is appended only if the user passed a bare path; preserves
# `./eng arg1 arg2` style commands intact.
if [[ "$ENG1" != *" "* ]]; then ENG1="$ENG1 uci"; fi
if [[ "$ENG2" != *" "* ]]; then ENG2="$ENG2 uci"; fi

mkdir -p runs

for PLY in $PLIES; do
    echo "=== ply ${PLY} ==="
    time python tools/match.py \
        --engine1 "$ENG1" \
        --engine2 "$ENG2" \
        --games "$GAMES" \
        --movetime "$TIME" \
        --depth "$PLY" \
        --max-moves 100 \
        $JOBS \
        > "wynikply${PLY}.dat"
    echo "po ${PLY}"
done

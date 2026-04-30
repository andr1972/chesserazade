#!/usr/bin/env bash
# Single-TC head-to-head with games count and movetime as positional
# arguments. Like test200.sh / test1000.sh but parametrised — saves
# adding a new test<N>.sh every time you want a different TC.
#
# Usage:
#   source <venv>/bin/activate    # python-chess required
#   ./testmt.sh <games> <movetime-ms> <engine1-cmd> <engine2-cmd>
#
# Examples:
#   ./testmt.sh 20 200 ./build/release/a ./build/release/b
#   ./testmt.sh 100 1000 ./build/release/chesserazade ./build/release/chesserazade_old
#
# Env-var overrides (optional):
#   JOBS=-j8             (default -j   = phys cores - 1)
set -euo pipefail

if [[ $# -lt 4 ]]; then
    echo "usage: $0 <games> <movetime-ms> <engine1-cmd> <engine2-cmd>" >&2
    exit 1
fi
GAMES="$1"
MS="$2"
ENG1="$3"
ENG2="$4"

JOBS="${JOBS:--j}"

mkdir -p runs

echo "=== ${GAMES} games × ${MS} ms ==="
time python tools/match.py \
    --engine1 "$ENG1" \
    --engine2 "$ENG2" \
    --games "$GAMES" \
    --movetime "$MS" \
    $JOBS
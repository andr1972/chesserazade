#!/usr/bin/env python3
# Copyright (c) 2024-2026 Andrzej Borucki
# SPDX-License-Identifier: Apache-2.0

"""Per-move blunder attribution between two engine builds.

Pipeline:
  1. Run a small head-to-head between --candidate and --reference.
  2. Pick the shortest game where the candidate lost.
  3. Replay every move the candidate made and ask the reference what
     it would have played in the same position (fresh ucinewgame so
     no carry-over TT).
  4. For each disagreement, run --analyzer (Stockfish) with MultiPV
     and report each candidate's deepest-observed rank.

Output is a table where '0' means 'not in top-K of any iteration' —
a strong signal that the move was an outright blunder by the oracle.

Usage:
    tools/blunder_hunt.py \\
        --candidate ./build/release/chesserazade \\
        --reference ./build/release/a \\
        --analyzer stockfish \\
        --games 10 --movetime 1000 --max-moves 70

Defaults are tuned for a ~2-3 min total run on 11 cores.
"""

from __future__ import annotations

import argparse
import io
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

import chess
import chess.pgn

REPO = Path(__file__).resolve().parent.parent
MATCH_PY = REPO / "tools" / "match.py"


# ----- Match runner: shell out to match.py and find shortest loss ----

def run_match(candidate: str, reference: str, *,
              games: int, movetime: int, max_moves: int,
              jobs: Optional[int]) -> Path:
    """Spawn match.py, return path to the resulting PGN."""
    cmd = [sys.executable, str(MATCH_PY),
           "--engine1", candidate, "--engine2", reference,
           "--games", str(games), "--movetime", str(movetime),
           "--max-moves", str(max_moves)]
    if jobs is None:
        cmd += ["-j"]
    else:
        cmd += ["-j", str(jobs)]
    print(f"# match: {games} games × {movetime} ms (cap {max_moves} fullmoves)",
          flush=True)
    res = subprocess.run(cmd, capture_output=True, text=True, check=True)
    # Last 'wrote <path>' line tells us where the PGN landed.
    pgn_path = None
    for line in res.stdout.splitlines():
        m = re.match(r"# wrote (.+\.pgn)", line)
        if m:
            pgn_path = Path(m.group(1))
        # Echo the final summary so the user sees match results too.
        if line.startswith("# === final") or line.startswith("# Elo"):
            print(line, flush=True)
    if pgn_path is None or not pgn_path.exists():
        raise RuntimeError("match.py did not report a PGN file")
    return pgn_path


def shortest_candidate_loss(pgn_path: Path,
                            candidate_name: str
                            ) -> Optional[chess.pgn.Game]:
    """Pick the shortest game in `pgn_path` where the candidate lost.
    Length measured in plies (move_stack length)."""
    best: Optional[chess.pgn.Game] = None
    best_plies = 10**9
    with pgn_path.open() as f:
        while True:
            game = chess.pgn.read_game(f)
            if game is None:
                break
            white = game.headers.get("White", "")
            black = game.headers.get("Black", "")
            result = game.headers.get("Result", "")
            cand_lost = (
                (result == "1-0" and black == candidate_name)
                or (result == "0-1" and white == candidate_name)
            )
            if not cand_lost:
                continue
            plies = sum(1 for _ in game.mainline_moves())
            if plies < best_plies:
                best = game
                best_plies = plies
    return best


# ----- UCI engine wrapper (minimal, single-position queries) ---------

class UciEngine:
    """One-shot UCI client. ucinewgame between every position so no
    TT/history bleed across queries — important for getting honest
    'what would you play here, fresh?' answers."""

    def __init__(self, cmd: str, multipv: int = 1) -> None:
        self.cmd = cmd
        self.proc = subprocess.Popen(
            shlex.split(cmd),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1,
        )
        self._send("uci")
        self._wait("uciok")
        if multipv > 1:
            self._send(f"setoption name MultiPV value {multipv}")
        self._send("isready")
        self._wait("readyok")

    def _send(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def _wait(self, token: str, timeout: float = 30.0) -> None:
        assert self.proc.stdout is not None
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"{self.cmd}: closed waiting '{token}'")
            if line.strip().startswith(token):
                return
        raise TimeoutError(f"{self.cmd}: no '{token}' in {timeout}s")

    def bestmove(self, fen: str, movetime_ms: int) -> str:
        """Return the engine's bestmove uci for the given FEN."""
        self._send("ucinewgame")
        self._send("isready")
        self._wait("readyok")
        self._send(f"position fen {fen}")
        self._send(f"go movetime {movetime_ms}")
        assert self.proc.stdout is not None
        deadline = time.time() + movetime_ms / 1000.0 + 5.0
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"{self.cmd}: closed mid-search")
            if line.startswith("bestmove"):
                tokens = line.split()
                return tokens[1] if len(tokens) > 1 else ""
        raise TimeoutError(f"{self.cmd}: no bestmove")

    def multipv_analysis(self, fen: str, movetime_ms: int
                         ) -> list[tuple[int, int, str]]:
        """Return all (depth, rank, move_uci) tuples observed during
        the search. Caller decides how to summarise — typically takes
        the deepest occurrence of each move."""
        self._send("ucinewgame")
        self._send("isready")
        self._wait("readyok")
        self._send(f"position fen {fen}")
        self._send(f"go movetime {movetime_ms}")
        observations: list[tuple[int, int, str]] = []
        assert self.proc.stdout is not None
        deadline = time.time() + movetime_ms / 1000.0 + 5.0
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"{self.cmd}: closed mid-search")
            line = line.strip()
            if line.startswith("info "):
                obs = parse_info_line(line)
                if obs is not None:
                    observations.append(obs)
            elif line.startswith("bestmove"):
                return observations
        raise TimeoutError(f"{self.cmd}: no bestmove")

    def quit(self) -> None:
        try:
            self._send("quit")
            self.proc.wait(timeout=2)
        except Exception:
            self.proc.kill()


_INFO_RE = re.compile(
    r"depth (\d+).*?multipv (\d+).*? pv (\S+)"
)


def parse_info_line(line: str) -> Optional[tuple[int, int, str]]:
    """Pull (depth, multipv-rank, first-pv-move) from one UCI info line.
    Returns None when the line doesn't have all three (e.g. 'info string',
    or non-multipv lines from engines that omit the multipv tag)."""
    m = _INFO_RE.search(line)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), m.group(3)


def deepest_rank(observations: list[tuple[int, int, str]],
                 move_uci: str) -> tuple[int, int]:
    """Return (deepest_depth_observed, rank_at_that_depth) for `move_uci`.
    Returns (0, 0) when the move never appeared in any iteration's
    top-K MultiPV. When a move appeared at multiple depths, takes the
    deepest. Within the deepest iteration, takes the smallest rank
    (best ranking)."""
    by_depth: dict[int, int] = {}  # depth -> best (smallest) rank seen
    for depth, rank, mv in observations:
        if mv != move_uci:
            continue
        prev = by_depth.get(depth)
        if prev is None or rank < prev:
            by_depth[depth] = rank
    if not by_depth:
        return (0, 0)
    deepest = max(by_depth)
    return (deepest, by_depth[deepest])


# ----- Main analysis ------------------------------------------------

def analyse_loss(game: chess.pgn.Game,
                 candidate_name: str,
                 reference: UciEngine,
                 analyzer: UciEngine,
                 *, ref_movetime: int, sf_movetime: int) -> None:
    """Walk the game move by move; for every move played by `candidate`,
    compare with `reference` on the same FEN, and rank both candidates
    on the analyzer when they disagree."""
    board = game.board()
    cand_is_white = (game.headers.get("White") == candidate_name)
    print(f"# game: {game.headers.get('White')} vs "
          f"{game.headers.get('Black')}  result {game.headers.get('Result')}",
          flush=True)
    print(f"# candidate plays {'white' if cand_is_white else 'black'};"
          f" comparing every candidate move against reference + analyzer",
          flush=True)
    print(f"{'ply':<4} {'cand':<8} {'ref':<8} "
          f"{'cand_depth':<10} {'cand_rank':<9} "
          f"{'ref_depth':<9} {'ref_rank':<8}", flush=True)

    for ply_idx, move in enumerate(game.mainline_moves(), start=1):
        cand_to_move = (board.turn == chess.WHITE) == cand_is_white
        if not cand_to_move:
            board.push(move)
            continue

        fen = board.fen()
        cand_uci = move.uci()

        # Ask reference what it would have played here.
        ref_uci = reference.bestmove(fen, ref_movetime)
        if ref_uci == cand_uci:
            print(f"{ply_idx:<4} {cand_uci:<8} = ", flush=True)
            board.push(move)
            continue

        # Disagreement — run the analyzer with MultiPV.
        observations = analyzer.multipv_analysis(fen, sf_movetime)
        cand_depth, cand_rank = deepest_rank(observations, cand_uci)
        ref_depth, ref_rank = deepest_rank(observations, ref_uci)
        print(f"{ply_idx:<4} {cand_uci:<8} {ref_uci:<8} "
              f"{cand_depth:<10} {cand_rank:<9} "
              f"{ref_depth:<9} {ref_rank:<8}", flush=True)
        board.push(move)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Find candidate-engine moves that the reference"
                    " disagreed with and the analyzer ranks low.")
    ap.add_argument("--candidate", required=True,
                    help="engine under test (e.g. ./build/release/chesserazade)")
    ap.add_argument("--reference", required=True,
                    help="baseline engine for alternatives (e.g. ./build/release/a)")
    ap.add_argument("--analyzer", default="stockfish",
                    help="strong engine used as oracle (default: stockfish)")
    ap.add_argument("--games", type=int, default=10)
    ap.add_argument("--movetime", type=int, default=1000,
                    help="ms per move during the match phase (default 1000)")
    ap.add_argument("--ref-movetime", type=int, default=None,
                    help="ms per move when querying the reference for"
                         " alternatives (default = --movetime)")
    ap.add_argument("--sf-movetime", type=int, default=1000,
                    help="ms per position for the analyzer (default 1000)")
    ap.add_argument("--max-moves", type=int, default=70,
                    help="full-move cap during the match phase, draws"
                         " marathons (default 70)")
    ap.add_argument("--multipv", type=int, default=20,
                    help="MultiPV count for the analyzer (default 20).")
    ap.add_argument("-j", "--jobs", type=int, default=None,
                    help="match.py parallel workers (default phys-1)")
    ap.add_argument("--pgn", type=Path, default=None,
                    help="skip the match phase and analyse this PGN"
                         " (must contain at least one candidate loss).")
    args = ap.parse_args()

    ref_mt = args.ref_movetime if args.ref_movetime is not None else args.movetime

    cand_name = Path(args.candidate.split()[0]).stem
    if args.pgn is None:
        pgn_path = run_match(
            args.candidate, args.reference,
            games=args.games, movetime=args.movetime,
            max_moves=args.max_moves, jobs=args.jobs)
    else:
        pgn_path = args.pgn

    game = shortest_candidate_loss(pgn_path, cand_name)
    if game is None:
        print(f"# no losses for '{cand_name}' in {pgn_path} — increase --games"
              f" or change positions.", file=sys.stderr)
        return 1
    print(f"# shortest loss: round {game.headers.get('Round')}, "
          f"{sum(1 for _ in game.mainline_moves())} ply", flush=True)

    print(f"# starting reference engine ({args.reference})...", flush=True)
    reference = UciEngine(args.reference, multipv=1)
    print(f"# starting analyzer ({args.analyzer})"
          f" with MultiPV {args.multipv}...", flush=True)
    analyzer = UciEngine(args.analyzer, multipv=args.multipv)

    try:
        analyse_loss(game, cand_name, reference, analyzer,
                     ref_movetime=ref_mt, sf_movetime=args.sf_movetime)
    finally:
        reference.quit()
        analyzer.quit()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
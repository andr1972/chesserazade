#!/usr/bin/env python3
# Copyright (c) 2024-2026 Andrzej Borucki
# SPDX-License-Identifier: Apache-2.0

"""Replay match-20260428-185535 game 4 deterministically.

Goal: reproduce the `std::bad_alloc` crash that aborted that game in
worker 1 around white move 94. The opening (4 random plies) is shared
with game 3, whose PGN survived: 1.f3 d6 2.a4 g6. In game 4 the
colors are swapped — chesserazade_old plays white, the new
chesserazade plays black.

Each move is followed by a FEN dump so when the crash hits we know
the exact position.
"""

from __future__ import annotations

import shlex
import subprocess
import sys
import time
from pathlib import Path

import chess


REPO = Path(__file__).resolve().parent.parent
NEW = str(REPO / "build/release/chesserazade")
OLD = str(REPO / "build/release/chesserazade_old")
MOVETIME_MS = 1000
OPENING_PLIES = ["f2f3", "d7d6", "a2a4", "g7g6"]


class Engine:
    def __init__(self, cmd: str, label: str) -> None:
        self.label = label
        self.proc = subprocess.Popen(
            shlex.split(cmd),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
        )
        self._send("uci")
        self._wait_for("uciok")
        self._send("isready")
        self._wait_for("readyok")

    def _send(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def _wait_for(self, token: str, timeout: float = 10.0) -> None:
        assert self.proc.stdout is not None
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"{self.label}: closed waiting for '{token}'")
            if line.strip().startswith(token):
                return
        raise TimeoutError(f"{self.label}: did not say '{token}' in {timeout}s")

    def go(self, board: chess.Board, movetime_ms: int) -> chess.Move:
        moves = " ".join(m.uci() for m in board.move_stack)
        if moves:
            self._send(f"position startpos moves {moves}")
        else:
            self._send("position startpos")
        self._send(f"go movetime {movetime_ms}")
        deadline = time.time() + (movetime_ms / 1000.0) + 5.0
        assert self.proc.stdout is not None
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                # Engine died. Drain stderr so the caller sees why.
                err = ""
                if self.proc.stderr is not None:
                    try:
                        err = self.proc.stderr.read()
                    except Exception:
                        pass
                raise RuntimeError(
                    f"{self.label}: engine closed mid-search\n--- stderr ---\n{err}")
            line = line.strip()
            if line.startswith("bestmove"):
                tokens = line.split()
                if len(tokens) < 2:
                    raise RuntimeError(f"{self.label}: malformed bestmove: {line!r}")
                return chess.Move.from_uci(tokens[1])
        raise TimeoutError(f"{self.label}: bestmove never arrived")

    def quit(self) -> None:
        try:
            self._send("quit")
            self.proc.wait(timeout=2)
        except Exception:
            self.proc.kill()


def main() -> int:
    for path in (NEW, OLD):
        if not Path(path).exists():
            print(f"missing binary: {path}", file=sys.stderr)
            return 1

    white = Engine(f"{OLD} uci", "old (white)")
    black = Engine(f"{NEW} uci", "new (black)")

    board = chess.Board()
    # Apply the shared opening — same sequence game 3 produced.
    for uci in OPENING_PLIES:
        m = chess.Move.from_uci(uci)
        if m not in board.legal_moves:
            print(f"opening move {uci} not legal — RNG drift?", file=sys.stderr)
            return 1
        board.push(m)
        print(f"opening: {uci}")
    print(f"FEN after opening: {board.fen()}\n")

    try:
        ply = 0
        while not board.is_game_over(claim_draw=True):
            engine = white if board.turn == chess.WHITE else black
            ply += 1
            full = (board.fullmove_number, "w" if board.turn == chess.WHITE else "b")
            t0 = time.time()
            move = engine.go(board, MOVETIME_MS)
            dt = time.time() - t0
            if move not in board.legal_moves:
                print(f"!! illegal move from {engine.label}: {move.uci()}",
                      file=sys.stderr)
                break
            print(f"ply {ply:3d}  ({full[0]}{full[1]}, {dt:.1f}s)  "
                  f"{engine.label}: {move.uci()}")
            board.push(move)
            print(f"FEN: {board.fen()}")
            sys.stdout.flush()
        print(f"\n=== game ended: {board.result(claim_draw=True)}")
    except Exception as exc:
        print(f"\n!! crash at ply {ply} ({full[0]}{full[1]}): {exc}",
              file=sys.stderr)
        print(f"!! last good FEN (before the crashing search):", file=sys.stderr)
        print(f"!! {board.fen()}", file=sys.stderr)
    finally:
        white.quit()
        black.quit()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

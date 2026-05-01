#!/usr/bin/env python3
# Copyright (c) 2024-2026 Andrzej Borucki
# SPDX-License-Identifier: Apache-2.0

"""Run a UCI engine match. Two engines, N games, fixed movetime.

Usage:
    python3 scripts/match.py \\
        --engine1 "/path/to/engine1 [args...]" \\
        --engine2 "/path/to/engine2 [args...]" \\
        --games 10 --movetime 1000

Each game alternates colors so engines play half as white and half as
black. Termination by checkmate, stalemate, threefold repetition,
50-move rule, or insufficient material — all detected by python-chess.

Output:
    - live result summary on stdout
    - PGN file at runs/match-<timestamp>.pgn
"""

from __future__ import annotations

import argparse
import datetime as dt
import math
import os
import queue
import random
import shlex
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from statistics import NormalDist
from typing import Callable, Optional

import chess
import chess.pgn


def elo_diff(score: float, n: float) -> float:
    """Logistic-Elo from a score percentage. ±inf at the endpoints
    (100% / 0%) — caller decides how to format infinities."""
    if n <= 0:
        return 0.0
    p = score / n
    if p <= 0.0:
        return -math.inf
    if p >= 1.0:
        return math.inf
    return -400.0 * math.log10(1.0 / p - 1.0)


def elo_interval(score: float, n: float, alpha: float = 0.05
                 ) -> tuple[float, float, float]:
    """Wilson-based confidence interval on the score percentage,
    converted to Elo. Returns (point, lo, hi) — any of which may be
    ±inf when the bound corresponds to score% 0 or 100. Wilson is
    chosen because it stays well-defined at the extremes (a plain
    Wald interval would collapse to width-zero at 20:0).

    Treats each game as a unit-trial; with draws the score is
    fractional, which makes the binomial assumption approximate but
    matches what BayesElo / Ordo / Cute-Chess all do in practice."""
    if n <= 0:
        return 0.0, -math.inf, math.inf
    z = NormalDist().inv_cdf(1.0 - alpha / 2.0)
    p_hat = score / n
    denom = 1.0 + z * z / n
    center = (p_hat + z * z / (2.0 * n)) / denom
    half = (z * math.sqrt(p_hat * (1.0 - p_hat) / n
                          + z * z / (4.0 * n * n))) / denom
    p_lo = max(0.0, center - half)
    p_hi = min(1.0, center + half)
    return elo_diff(score, n), elo_diff(p_lo * n, n), elo_diff(p_hi * n, n)


def format_elo(score1: float, score2: float, *,
               confidences: tuple[int, ...] = (50, 75, 95)) -> str:
    """One-line Elo summary with several confidence intervals so the
    reader can pick the level they care about. 95% is convention,
    50% / 75% give a tighter-looking band that's still mathematically
    honest — the trade-off is just how often the true Elo would land
    inside it across repeated experiments. At extreme scores (one
    side ran the table) the point estimate is ±inf, so we report
    one-sided bounds at each level instead."""
    n = score1 + score2
    point = elo_diff(score1, n)

    def fmt_bound(val: float) -> str:
        if math.isinf(val):
            return "-inf" if val < 0 else "+inf"
        return f"{val:+.0f}"

    parts = []
    for pct in confidences:
        alpha = 1.0 - pct / 100.0
        _, lo, hi = elo_interval(score1, n, alpha=alpha)
        if math.isinf(point) and point > 0:
            parts.append(f"{pct}%: >{fmt_bound(lo)}")
        elif math.isinf(point) and point < 0:
            parts.append(f"{pct}%: <{fmt_bound(hi)}")
        else:
            parts.append(f"{pct}%: [{fmt_bound(lo)}, {fmt_bound(hi)}]")

    if math.isinf(point):
        # Point estimate is undefined (one side ran the table); the
        # one-sided bounds in `parts` carry the whole signal.
        head = "Elo:"
    else:
        head = f"Elo: {point:+.0f}"
    return head + "  " + "  ".join(parts)


def match_verdict(score1: float, score2: float, draws: int,
                  name1: str = "1", name2: str = "2", *,
                  alpha: float = 0.05) -> str:
    """Decide whether a head-to-head score is statistically decisive.

    Under H0 (equal strength) decisive games split Binomial(d, 1/2),
    so the score-difference z-statistic is
        z = (score1 - N/2) · 2 / √decisive
    with N the total games and `decisive` = N − draws. We reject H0
    at confidence 1 − alpha when |z| exceeds the two-sided normal
    quantile.

    Returns one of:
      'undecided (all draws)'
      'undecided (|z|=… < z_crit=…)'
      'Winner: <name> (|z|=…, p≈…)'
    """
    n = score1 + score2
    decisive = int(round(n)) - draws
    if decisive <= 0:
        return "undecided (all draws)"
    z = (score1 - n / 2.0) * 2.0 / math.sqrt(decisive)
    z_crit = NormalDist().inv_cdf(1.0 - alpha / 2.0)
    if abs(z) <= z_crit:
        return (f"undecided (|z|={abs(z):.2f} < z_crit={z_crit:.2f}"
                f" at α={alpha})")
    p_value = 2.0 * (1.0 - NormalDist().cdf(abs(z)))
    winner = name1 if z > 0 else name2
    return f"Winner: {winner} (|z|={abs(z):.2f}, p≈{p_value:.3f})"


def physical_core_cpus() -> list[int]:
    """One representative logical-CPU id per *physical* core, in the
    order they appear in /proc/cpuinfo. SMT siblings of the same
    physical core are dropped — we keep just the first. Empty list
    if /proc/cpuinfo can't be parsed (non-Linux, sandboxed)."""
    seen: dict[tuple[str, str], int] = {}
    try:
        with open("/proc/cpuinfo") as f:
            proc = pid = cid = None
            for line in f:
                if line.startswith("processor"):
                    proc = int(line.split(":")[1].strip())
                elif line.startswith("physical id"):
                    pid = line.split(":")[1].strip()
                elif line.startswith("core id"):
                    cid = line.split(":")[1].strip()
                elif line.strip() == "":
                    if proc is not None and pid is not None and cid is not None:
                        seen.setdefault((pid, cid), proc)
                    proc = pid = cid = None
    except Exception:
        return []
    return list(seen.values())


def physical_cores() -> int:
    """Number of distinct *physical* CPU cores. Falls back to logical
    `os.cpu_count()` if `/proc/cpuinfo` can't be parsed."""
    cpus = physical_core_cpus()
    if cpus:
        return len(cpus)
    return os.cpu_count() or 1


def load_epd_openings(path: Path) -> list[str]:
    """Read an EPD opening book and return one FEN per line.
    EPD's six fields are board, side, castling, en-passant, then
    optional opcodes (`id "..."`, `bm ...`, …) — same as the first
    four FEN fields without the halfmove / fullmove counters. We
    drop the opcodes and append '0 1' so python-chess accepts the
    string as a FEN. Blank and `#`-comment lines are skipped."""
    fens: list[str] = []
    with path.open() as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            # Cut off opcodes (split on the first ';' or after 4 fields).
            head = line.split(";", 1)[0].strip()
            tokens = head.split()
            if len(tokens) < 4:
                continue
            fens.append(" ".join(tokens[:4]) + " 0 1")
    if not fens:
        raise RuntimeError(f"no positions parsed from {path}")
    return fens


def random_opening(plies: int, rng: random.Random) -> chess.Board:
    """Walk `plies` random legal moves from startpos. Avoid positions
    that are already terminated (mate / stalemate / repetition)."""
    while True:
        board = chess.Board()
        ok = True
        for _ in range(plies):
            moves = list(board.legal_moves)
            if not moves or board.is_game_over():
                ok = False
                break
            board.push(rng.choice(moves))
        if ok and not board.is_game_over():
            return board


REPO = Path(__file__).resolve().parent.parent


def _binary_path_from_cmd(cmd: str) -> Path:
    """Extract the engine binary path from a UCI command line.

    For `env RUKH_NNUE=/x/y/z.nnue /path/to/rukh` we want
    `/path/to/rukh`, not the literal `env`. Skip a leading `env`
    plus any KEY=VALUE assignments, then return the next token.
    Returns an empty Path when nothing parseable is left.
    """
    tokens = shlex.split(cmd)
    i = 0
    if tokens and tokens[0] == "env":
        i = 1
        while i < len(tokens) and "=" in tokens[i]:
            i += 1
    if i >= len(tokens):
        return Path()
    return Path(tokens[i])


def _default_name_from_cmd(cmd: str) -> str:
    """Default engine name = binary basename without extension."""
    p = _binary_path_from_cmd(cmd)
    if p == Path():
        return cmd.split()[0]
    return p.stem


def _resolve_names(cmd1: str, cmd2: str,
                   name1: Optional[str], name2: Optional[str]
                   ) -> tuple[str, str]:
    """Pick display names for the two engines.

    If the user passed explicit `--name1` / `--name2`, those win
    unconditionally. Otherwise derive from the binary name. When
    both default names collide (a typical A/B test of the same
    binary built two ways — `build/release/chesserazade` vs
    `build/release-optim/chesserazade`) prepend the parent
    directory so the score lines stay distinguishable.
    """
    if name1 is not None and name2 is not None:
        return name1, name2
    p1 = _binary_path_from_cmd(cmd1)
    p2 = _binary_path_from_cmd(cmd2)
    n1 = name1 or p1.stem or cmd1.split()[0]
    n2 = name2 or p2.stem or cmd2.split()[0]
    if n1 == n2 and name1 is None and name2 is None:
        # Disambiguate by parent directory.
        parent1 = p1.parent.name or "1"
        parent2 = p2.parent.name or "2"
        if parent1 != parent2:
            # Names collide on the binary stem so the *parent
            # directory* is the only distinguishing piece — show
            # just that, not "parent/binary", to keep score
            # lines compact in A/B sweeps like
            # `build/release-optim/chesserazade` vs
            # `build/release/chesserazade`.
            n1 = parent1
            n2 = parent2
        else:
            # Same path? Last-resort tag.
            n1 = f"{n1}#1"
            n2 = f"{n2}#2"
    return n1, n2


class Engine:
    def __init__(self, cmd: str, name: Optional[str] = None,
                 pin_cpu: Optional[int] = None) -> None:
        self.cmd = cmd
        # stderr → /tmp/<name>-<pid>.err so a crashing engine
        # leaves an audit trail (assertion text, sanitizer report,
        # last UCI command before the close). Read it after a
        # crash to figure out what killed the subprocess.
        self.stderr_path = Path(
            f"/tmp/match-{Path(shlex.split(cmd)[0]).name}-"
            f"{int(time.time()*1000)}-{id(self)}.err")
        self._stderr_file = open(self.stderr_path, "w")
        argv = shlex.split(cmd)
        if pin_cpu is not None:
            # Pin both engines of a worker to the same physical
            # core so the kernel time-slices them rather than
            # letting them fight other workers' engines for cache.
            # Each game-pair then runs on a predictable, isolated
            # core with no cross-pair contention.
            argv = ["taskset", "-c", str(pin_cpu)] + argv
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self._stderr_file, text=True, bufsize=1,
        )
        self.name = name or _default_name_from_cmd(cmd)
        self.last_depth = 0  # set by go() per move
        # Aggregated counters parsed from `info string key=N` lines
        # in the most recent `go`. Reset at the start of every go.
        self.last_info: dict[str, int] = {}
        # Cumulative across the engine's whole lifetime — handy for
        # match-end summaries that want totals over hundreds of games.
        self.cum_info: dict[str, int] = {}
        self._suspended = False
        self._send("uci")
        self._wait_for("uciok", timeout=10)
        self._send("isready")
        self._wait_for("readyok", timeout=10)

    def suspend(self) -> None:
        """SIGSTOP the engine. Used when the opponent is to move so a
        rogue engine can't burn CPU off-clock (pondering, busy loops).
        Best-effort: silently ignored on platforms without SIGSTOP or
        if the process is already gone."""
        if self._suspended or self.proc.poll() is not None:
            return
        try:
            self.proc.send_signal(signal.SIGSTOP)
            self._suspended = True
        except Exception:
            pass

    def resume(self) -> None:
        if not self._suspended or self.proc.poll() is not None:
            return
        try:
            self.proc.send_signal(signal.SIGCONT)
            self._suspended = False
        except Exception:
            pass

    def _send(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def _wait_for(self, token: str, *, timeout: float) -> str:
        assert self.proc.stdout is not None
        deadline = time.time() + timeout
        last = ""
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"{self.name}: engine closed waiting for '{token}'")
            line = line.strip()
            last = line
            if line.startswith(token) or token in line.split():
                return line
        raise TimeoutError(f"{self.name}: did not say '{token}' in {timeout}s; last={last!r}")

    def new_game(self) -> None:
        # Engine must be running to handle ucinewgame — previous
        # game may have left it suspended on the loser's side.
        self.resume()
        self._send("ucinewgame")
        self._send("isready")
        self._wait_for("readyok", timeout=10)

    def go(self, board: chess.Board, movetime_ms: int,
           depth: Optional[int] = None,
           start_fen: Optional[str] = None) -> chess.Move:
        # If a custom starting position is supplied (EPD-book opening),
        # send `position fen ...` instead of the default `startpos`.
        # Either form accepts a `moves ...` suffix for plies played
        # from that root.
        moves = " ".join(m.uci() for m in board.move_stack)
        prefix = (f"position fen {start_fen}" if start_fen is not None
                  else "position startpos")
        if moves:
            self._send(f"{prefix} moves {moves}")
        else:
            self._send(prefix)
        # Both depth and movetime can be sent — search stops at
        # whichever fires first.
        cmd = f"go movetime {movetime_ms}"
        if depth is not None and depth > 0:
            cmd += f" depth {depth}"
        self._send(cmd)
        # Engine sends some 'info ...' lines, then 'bestmove <m>'.
        # Track the deepest "info depth N" we see during the
        # search so the caller can read `self.last_depth` after
        # `go` returns. Reset to 0 on every go so an unanswered
        # depth probe shows up explicitly.
        self.last_depth = 0
        self.last_info = {}
        deadline = time.time() + (movetime_ms / 1000.0) + 5.0
        assert self.proc.stdout is not None
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"{self.name}: engine closed mid-search")
            line = line.strip()
            if line.startswith("info "):
                # `info string ...` is a free-form UCI comment — the
                # engine prints its NMP/verify counters there. Sum
                # `key=N` tokens into per-search totals so the
                # caller can read `engine.last_info` after `go`.
                if line.startswith("info string "):
                    body = line[len("info string "):]
                    for tok in body.split():
                        if "=" in tok:
                            k, v = tok.split("=", 1)
                            try:
                                self.last_info[k] = (
                                    self.last_info.get(k, 0) + int(v))
                            except ValueError:
                                pass
                    continue
                # UCI 'info ... depth N ...' — N is the next token.
                tokens = line.split()
                for k, tok in enumerate(tokens):
                    if tok == "depth" and k + 1 < len(tokens):
                        try:
                            d = int(tokens[k + 1])
                        except ValueError:
                            break
                        if d > self.last_depth:
                            self.last_depth = d
                        break
                continue
            if line.startswith("bestmove"):
                tokens = line.split()
                if len(tokens) < 2:
                    raise RuntimeError(f"{self.name}: malformed bestmove: {line!r}")
                for k, v in self.last_info.items():
                    self.cum_info[k] = self.cum_info.get(k, 0) + v
                return chess.Move.from_uci(tokens[1])
        raise TimeoutError(f"{self.name}: bestmove never arrived")

    def quit(self) -> None:
        try:
            # Must be running to receive `quit` and exit cleanly.
            self.resume()
            self._send("quit")
            self.proc.wait(timeout=2)
        except Exception:
            self.proc.kill()
        finally:
            try:
                self._stderr_file.close()
            except Exception:
                pass
            # Drop empty stderr logs so /tmp doesn't fill up with
            # zero-byte files from clean exits — keep only the
            # ones that actually captured output.
            try:
                if self.stderr_path.stat().st_size == 0:
                    self.stderr_path.unlink()
            except Exception:
                pass


def play_game(white: Engine, black: Engine, *,
              movetime_white: int, movetime_black: int,
              depth_white: Optional[int] = None,
              depth_black: Optional[int] = None,
              opening: Optional[chess.Board] = None,
              game_label: str = "",
              status_ref: Optional[dict] = None,
              fen_log: Optional[Callable[[str], None]] = None,
              max_moves: int = 0
              ) -> tuple[str, chess.Board, str]:
    """Play one game. Returns (result_string, final_board, termination_reason).

    `game_label` (e.g. "[3/10]") is prefixed to every per-move log line so
    a tail -f can attribute moves to the right game in a multi-game match.

    `status_ref`, when supplied, gets `status_ref["moves"]` updated to the
    current full-move number after each move — the heartbeat thread reads
    this to report live progress without holding the per-move loop's
    print lock.
    """
    white.new_game()
    black.new_game()
    # Rebuild from the opening's FEN with an empty move_stack so the
    # UCI position string only carries plies played in *this* game.
    # For EPD openings the FEN is the literal book entry; for the
    # random-plies generator it's the post-walk position. Engines
    # see a clean game starting from start_fen, regardless of source.
    if opening is not None:
        start_fen = opening.fen()
        board = chess.Board(start_fen)
    else:
        start_fen = None  # plain startpos
        board = chess.Board()
    if status_ref is not None:
        status_ref["last_move"] = "—"
    if fen_log is not None:
        fen_log(f"start {white.name} vs {black.name}  fen {board.fen()}")
    if game_label:
        # Preserve the original opening trail / FEN label even though
        # we re-anchored the search board with an empty move_stack:
        # random-walk openings have moves to print, EPD openings have
        # only a FEN.
        if opening is not None and opening.move_stack:
            opening_str = " ".join(m.uci() for m in opening.move_stack)
            print(f"{game_label} opening: {opening_str}", flush=True)
        elif opening is not None:
            print(f"{game_label} opening: fen {start_fen}", flush=True)
        sys.stdout.write(f"{game_label} ")
        sys.stdout.flush()
    while not board.is_game_over(claim_draw=True):
        # Adjudicated draw cap: very long games (175+ full moves)
        # explode UCI overhead — the `position startpos moves ...`
        # token list grows linearly and parsing dominates by the
        # endgame. After `max_moves`, treat as a draw so the
        # match progresses.
        if max_moves > 0 and board.fullmove_number > max_moves:
            if game_label:
                sys.stdout.write("\n"); sys.stdout.flush()
            return "1/2-1/2", board, "MAX_MOVES"
        engine = white if board.turn == chess.WHITE else black
        idle = black if board.turn == chess.WHITE else white
        movetime_ms = movetime_white if board.turn == chess.WHITE else movetime_black
        depth_cap = depth_white if board.turn == chess.WHITE else depth_black
        # Freeze the side that isn't on move so it physically can't
        # burn CPU off-clock (rogue ponder, `while true` busy-wait).
        # On a single shared core (taskset pinning), this matters: a
        # cheating opponent could otherwise eat half our budget.
        idle.suspend()
        engine.resume()
        move = engine.go(board, movetime_ms, depth_cap,
                         start_fen=start_fen)
        if move not in board.legal_moves:
            # Forfeit: engine returned an illegal move.
            losing_color = "White" if board.turn == chess.WHITE else "Black"
            result = "0-1" if board.turn == chess.WHITE else "1-0"
            if game_label:
                sys.stdout.write("\n"); sys.stdout.flush()
            return result, board, f"illegal move from {losing_color}: {move.uci()}"
        if game_label:
            sys.stdout.write(".")
            sys.stdout.flush()
        board.push(move)
        # `board.turn` is now whose move comes *next*; tag the
        # log/status with the side that just played.
        if board.turn == chess.WHITE:
            tag = f"{board.fullmove_number - 1}b"
        else:
            tag = f"{board.fullmove_number}w"
        if status_ref is not None:
            status_ref["last_move"] = tag
        if fen_log is not None:
            fen_log(f"{tag} {move.uci()} depth {engine.last_depth}  "
                    f"fen {board.fen()}")
    if game_label:
        sys.stdout.write("\n")
        sys.stdout.flush()

    # python-chess result string is the canonical PGN result.
    return board.result(claim_draw=True), board, board.outcome(claim_draw=True).termination.name


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine1", required=True, help="UCI command for engine 1")
    ap.add_argument("--engine2", required=True, help="UCI command for engine 2")
    ap.add_argument("--name1", default=None)
    ap.add_argument("--name2", default=None)
    ap.add_argument("--games", type=int, default=10)
    ap.add_argument("--movetime", type=int, default=1000, help="ms per move (default for both)")
    ap.add_argument("--movetime1", type=int, default=None,
                    help="override movetime for engine 1 (handicap matches)")
    ap.add_argument("--movetime2", type=int, default=None,
                    help="override movetime for engine 2")
    ap.add_argument("--depth", type=int, default=None,
                    help="hard ply cap per move (search stops at"
                         " whichever fires first: this depth or movetime)."
                         " Default: no extra cap (movetime alone).")
    ap.add_argument("--depth1", type=int, default=None,
                    help="override --depth for engine 1 (handicap matches)")
    ap.add_argument("--depth2", type=int, default=None,
                    help="override --depth for engine 2")
    ap.add_argument("--max-moves", type=int, default=0,
                    help="adjudicate as a draw after this many full moves"
                         " — keeps games-per-side bounded so a single"
                         " marathon endgame doesn't stall the match."
                         " Default 0 = no cap (rely on FIDE rules).")
    ap.add_argument("--random-plies", type=int, default=4,
                    help="Random plies played from startpos before the engines"
                         " take over. Pairs of games share the same opening,"
                         " played from each side. 0 = pure startpos. Ignored"
                         " when --openings is set.")
    ap.add_argument("--openings", type=Path, default=None,
                    help="EPD file of starting positions (one position per"
                         " line, first four FEN fields are enough). Each pair"
                         " of games shares one position picked at random from"
                         " the file (paired-openings: same position from each"
                         " side). Overrides --random-plies. Get a curated"
                         " book at github.com/official-stockfish/books, e.g."
                         " noob_3moves.epd.")
    ap.add_argument("--seed", type=int, default=12345,
                    help="seed for random openings (reproducible matches)")
    ap.add_argument("--pgn", type=Path, default=None,
                    help="output PGN; default runs/match-<timestamp>.pgn")
    ap.add_argument("-j", "--jobs", nargs="?", type=int, const=-1, default=1,
                    help="parallel games (make-style). No flag = serial."
                         " -j alone = (physical cores - 1), leaving one core"
                         " for match.py and OS noise so pinned worker cores"
                         " stay clean. -jN = N workers, capped the same way"
                         " and at the number of games.")
    ap.add_argument("--log", type=Path, default=None,
                    help="append a line per move (and per game start)"
                         " containing the resulting FEN. If a worker's engine"
                         " crashes, the last log line names the position the"
                         " engine was about to search. Lines from concurrent"
                         " games are interleaved but each is prefixed with"
                         " the game index.")
    ap.add_argument("--no-pin", action="store_true",
                    help="disable CPU pinning. By default each parallel"
                         " worker is pinned to one physical core (via"
                         " taskset) so its two engines time-slice on that"
                         " core without contending with other workers.")
    ap.add_argument("--heartbeat-interval", type=float, default=30.0,
                    help="seconds of console silence before the next heartbeat"
                         " line is printed in parallel mode (default 30)."
                         " Each game-result line resets the timer, so during"
                         " a busy run heartbeats stay quiet. Set to 0 to"
                         " disable entirely.")
    args = ap.parse_args()

    phys = physical_cores()
    # Leave one physical core free for match.py itself + OS noise.
    # With every core pinned to a worker pair, background load
    # (~20% on a typical desktop) lands on whichever pinned core is
    # handy and slows that pair's *currently-running* engine, biasing
    # the result. Capping at phys-1 keeps engine cores clean.
    job_cap = max(1, phys - 1) if phys >= 2 else 1
    if args.jobs == -1:
        jobs = job_cap
    else:
        jobs = args.jobs
    jobs = max(1, min(jobs, job_cap, args.games))
    # One physical-core CPU id per worker for pinning. Empty list
    # disables pinning (e.g. /proc/cpuinfo unavailable, or --no-pin).
    pin_cpus: list[int] = []
    if not args.no_pin and jobs > 1:
        pin_cpus = physical_core_cpus()[:jobs]
    # Randomise per-worker spawn order (e1-first vs e2-first) so any
    # micro-advantage to the first-spawned process averages out
    # instead of pinning to a fixed half of the workers. Seeded from
    # --seed + offset so runs stay reproducible and the stream is
    # independent of the openings RNG.
    spawn_e1_first = [random.Random(args.seed + 1 + w).random() < 0.5
                      for w in range(jobs)]

    # Resolve names without spawning yet — used in the header and as
    # the canonical keys for `score` / `games_pgn` (every parallel
    # worker creates its own engine pair with the same name).
    # Auto-disambiguates when both binaries share a basename.
    name1, name2 = _resolve_names(args.engine1, args.engine2,
                                  args.name1, args.name2)

    if args.pgn is None:
        ts = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        args.pgn = REPO / "runs" / f"match-{ts}.pgn"
    args.pgn.parent.mkdir(parents=True, exist_ok=True)

    mt1 = args.movetime1 if args.movetime1 is not None else args.movetime
    mt2 = args.movetime2 if args.movetime2 is not None else args.movetime
    dp1 = args.depth1 if args.depth1 is not None else args.depth
    dp2 = args.depth2 if args.depth2 is not None else args.depth
    rng = random.Random(args.seed)
    # Pre-generate one opening per *pair* of games so we can play it twice
    # (once from each side) — gives a fair sample at half the openings cost.
    pair_count = (args.games + 1) // 2
    if args.openings is not None:
        epd_fens = load_epd_openings(args.openings)
        openings = [chess.Board(rng.choice(epd_fens))
                    for _ in range(pair_count)]
    elif args.random_plies > 0:
        openings = [random_opening(args.random_plies, rng)
                    for _ in range(pair_count)]
    else:
        openings = None

    pin_desc = (f", pinned to CPUs {pin_cpus}" if pin_cpus
                else (", no pin" if jobs > 1 else ""))
    print(f"# {name1} vs {name2} — {args.games} games × {args.movetime}ms/move "
          f"(jobs={jobs}, physical cores={phys}{pin_desc})", flush=True)

    score: dict[str, float] = {name1: 0.0, name2: 0.0}
    # `info string key=N` totals merged across all worker instances
    # of each engine. Filled at worker cleanup (under `lock`) so the
    # main thread can summarise NMP/etc counters at match end.
    match_info: dict[str, dict[str, int]] = {name1: {}, name2: {}}
    games_pgn: list[Optional[chess.pgn.Game]] = [None] * args.games
    lock = threading.Lock()
    log_lock = threading.Lock()
    work_q: queue.Queue[int] = queue.Queue()
    for i in range(args.games):
        work_q.put(i)

    log_file = None
    if args.log is not None:
        args.log.parent.mkdir(parents=True, exist_ok=True)
        # Line buffering so a crashing worker still flushes its
        # most recent line — with default block buffering the
        # last few moves before the crash would be lost.
        log_file = open(args.log, "a", buffering=1)
        log_file.write(
            f"# match start {dt.datetime.now().isoformat()}  "
            f"{name1} vs {name2}  movetime {args.movetime}ms  "
            f"games {args.games}  jobs {jobs}\n")

    # Per-worker live status — keys are worker ids (0..jobs-1).
    # `None` means the worker is between games / done. Each
    # entry while a game is in progress holds the game index,
    # the move number (updated from `play_game`), and the wall-
    # clock start. The heartbeat thread reads these under
    # `lock`; workers update the dict without locking on the
    # hot per-move path (single-writer per slot makes that safe).
    worker_status: dict[int, Optional[dict]] = {w: None for w in range(jobs)}
    done_event = threading.Event()
    # Timestamp of the last line written to stdout (header counts).
    # The heartbeat thread waits until `now - last_print_ts >=
    # heartbeat_interval` before printing, so a busy run with
    # frequent game completions stays quiet.
    last_print_ts = [time.time()]

    def play_one(e1: Engine, e2: Engine, i: int, wid: int) -> None:
        # Alternate colors: even index → e1 white, odd → e2 white.
        white, black = (e1, e2) if (i % 2 == 0) else (e2, e1)
        opening = openings[i // 2] if openings is not None else None
        # Per-move dots only make sense in serial mode; in parallel
        # they would interleave between concurrent games (the
        # heartbeat thread reports progress instead).
        label = f"[{i+1}/{args.games}]" if jobs == 1 else ""
        t0 = time.time()
        status_entry = {"game": i, "last_move": "—", "started": t0}
        worker_status[wid] = status_entry

        fen_log = None
        if log_file is not None:
            game_index = i + 1
            def fen_log(line: str) -> None:
                with log_lock:
                    log_file.write(f"[g{game_index}] {line}\n")

        try:
            mt_white = mt1 if white is e1 else mt2
            mt_black = mt1 if black is e1 else mt2
            dp_white = dp1 if white is e1 else dp2
            dp_black = dp1 if black is e1 else dp2
            result, board, term = play_game(white, black,
                                            movetime_white=mt_white,
                                            movetime_black=mt_black,
                                            depth_white=dp_white,
                                            depth_black=dp_black,
                                            opening=opening,
                                            game_label=label,
                                            status_ref=status_entry,
                                            fen_log=fen_log,
                                            max_moves=args.max_moves)
        finally:
            worker_status[wid] = None
        dt_s = time.time() - t0
        with lock:
            if result == "1-0":
                score[white.name] += 1.0
            elif result == "0-1":
                score[black.name] += 1.0
            else:
                score[white.name] += 0.5
                score[black.name] += 0.5
            print(f"[{i+1}/{args.games}] {white.name} - {black.name}: {result} "
                  f"({term}, {board.fullmove_number} moves, {dt_s:.1f}s) | "
                  f"{name1} {score[name1]:.1f} - {score[name2]:.1f} {name2}",
                  flush=True)
            last_print_ts[0] = time.time()
            if fen_log is not None:
                fen_log(f"end {result} ({term})")
            game = chess.pgn.Game.from_board(board)
            game.headers["Event"] = "Rukh vs chesserazade match"
            game.headers["Round"] = str(i + 1)
            game.headers["White"] = white.name
            game.headers["Black"] = black.name
            game.headers["Result"] = result
            game.headers["TimeControl"] = f"{args.movetime}ms/move"
            games_pgn[i] = game

    def worker(wid: int) -> None:
        # `e1` / `e2` may be replaced mid-loop after an engine
        # crash (see the except below), so they're declared
        # outside the loop and the cleanup at the end re-reads
        # whatever is current.
        pin = pin_cpus[wid] if wid < len(pin_cpus) else None
        # Spawn order randomised per worker (see `spawn_e1_first`)
        # so the first-spawned-process micro-advantage averages out
        # rather than landing on a deterministic half of workers.
        if spawn_e1_first[wid]:
            e1: Optional[Engine] = Engine(args.engine1, name1, pin_cpu=pin)
            e2: Optional[Engine] = Engine(args.engine2, name2, pin_cpu=pin)
        else:
            e2 = Engine(args.engine2, name2, pin_cpu=pin)
            e1 = Engine(args.engine1, name1, pin_cpu=pin)
        try:
            while True:
                try:
                    i = work_q.get_nowait()
                except queue.Empty:
                    return
                try:
                    play_one(e1, e2, i, wid)
                except (RuntimeError, TimeoutError) as exc:
                    # Likely an engine subprocess died (segfault,
                    # assertion, mid-search close). Drop the game
                    # — exclude it from PGN and from the score —
                    # and respawn fresh engines so the worker can
                    # keep pulling from the queue.
                    stderr_paths = [str(e.stderr_path)
                                    for e in (e1, e2)
                                    if e is not None]
                    with lock:
                        print(f"[{i+1}/{args.games}] worker {wid}: "
                              f"game aborted ({exc}); engine stderr at: "
                              + ", ".join(stderr_paths),
                              flush=True)
                        last_print_ts[0] = time.time()
                        worker_status[wid] = None
                    if log_file is not None:
                        with log_lock:
                            log_file.write(
                                f"[g{i+1}] !! aborted: {exc}; "
                                f"stderr {','.join(stderr_paths)}\n")
                    for e in (e1, e2):
                        if e is not None:
                            try:
                                e.quit()
                            except Exception:
                                pass
                    if spawn_e1_first[wid]:
                        e1 = Engine(args.engine1, name1, pin_cpu=pin)
                        e2 = Engine(args.engine2, name2, pin_cpu=pin)
                    else:
                        e2 = Engine(args.engine2, name2, pin_cpu=pin)
                        e1 = Engine(args.engine1, name1, pin_cpu=pin)
        finally:
            for e in (e1, e2):
                if e is not None:
                    if e.cum_info and e.name in match_info:
                        with lock:
                            bucket = match_info[e.name]
                            for k, v in e.cum_info.items():
                                bucket[k] = bucket.get(k, 0) + v
                    try:
                        e.quit()
                    except Exception:
                        pass

    def heartbeat() -> None:
        # Idle-timer semantics: print only when the console has
        # been silent for `interval` seconds, then reset the
        # timer. Game-result and abort lines also touch
        # `last_print_ts`, so a busy match stays quiet and the
        # heartbeat surfaces only when something is genuinely
        # taking a while (long late-game endgame, stuck engine).
        interval = args.heartbeat_interval
        # Tick small enough to react to `done_event` without a
        # noticeable shutdown lag, capped by the interval so very
        # short intervals still work.
        tick = min(1.0, interval)
        while not done_event.wait(timeout=tick):
            now = time.time()
            if now - last_print_ts[0] < interval:
                continue
            with lock:
                done = sum(1 for g in games_pgn if g is not None)
                parts = []
                for w in sorted(worker_status):
                    s = worker_status[w]
                    if s is None:
                        continue
                    elapsed = now - s["started"]
                    parts.append(f"W{w}: g{s['game']+1} m{s['last_move']} "
                                 f"({elapsed:.0f}s)")
                if parts:
                    print(f"[heartbeat] done {done}/{args.games} | "
                          + " | ".join(parts), flush=True)
                    last_print_ts[0] = time.time()

    hb_thread: Optional[threading.Thread] = None
    try:
        if jobs > 1 and args.heartbeat_interval > 0:
            hb_thread = threading.Thread(target=heartbeat, daemon=True)
            hb_thread.start()

        if jobs == 1:
            worker(0)
        else:
            threads = [threading.Thread(target=worker, args=(w,), daemon=True)
                       for w in range(jobs)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()

    finally:
        # Stop the heartbeat thread regardless of how we got here
        # (normal completion, KeyboardInterrupt, engine crash).
        done_event.set()
        if hb_thread is not None:
            hb_thread.join(timeout=2.0)
        completed = [g for g in games_pgn if g is not None]
        with args.pgn.open("w") as f:
            for g in completed:
                print(g, file=f, end="\n\n")
        s1, s2 = score[name1], score[name2]
        played = s1 + s2
        # Decisive wins / losses / draws from e1's side, derived from the
        # PGN headers. (Avoid recounting from raw results to keep this in
        # sync with `score` updates above.)
        wins = sum(1 for g in completed
                   if (g.headers["Result"] == "1-0" and g.headers["White"] == name1)
                   or (g.headers["Result"] == "0-1" and g.headers["Black"] == name1))
        losses = sum(1 for g in completed
                     if (g.headers["Result"] == "0-1" and g.headers["White"] == name1)
                     or (g.headers["Result"] == "1-0" and g.headers["Black"] == name1))
        draws = sum(1 for g in completed if g.headers["Result"] == "1/2-1/2")
        print(f"\n# === final: {name1} {s1:.1f} - {s2:.1f} {name2}  "
              f"({wins}W / {draws}D / {losses}L of {played:.0f})", flush=True)
        print(f"# {match_verdict(s1, s2, draws, name1, name2)}", flush=True)
        print(f"# {format_elo(s1, s2)}", flush=True)
        for nm in (name1, name2):
            info = match_info.get(nm) or {}
            if info:
                summary = " ".join(f"{k}={v}" for k, v in sorted(info.items()))
                print(f"# {nm} info: {summary}", flush=True)
        print(f"# wrote {args.pgn}", flush=True)
        if log_file is not None:
            log_file.write(
                f"# match end {dt.datetime.now().isoformat()}  "
                f"final {name1} {s1:.1f} - {s2:.1f} {name2}\n")
            try:
                log_file.close()
            except Exception:
                pass
            print(f"# fen log {args.log}", flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

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
import os
import queue
import random
import shlex
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Callable, Optional

import chess
import chess.pgn


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
        self._send("uci")
        self._wait_for("uciok", timeout=10)
        self._send("isready")
        self._wait_for("readyok", timeout=10)

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
        self._send("ucinewgame")
        self._send("isready")
        self._wait_for("readyok", timeout=10)

    def go(self, board: chess.Board, movetime_ms: int,
           depth: Optional[int] = None) -> chess.Move:
        moves = " ".join(m.uci() for m in board.move_stack)
        if moves:
            self._send(f"position startpos moves {moves}")
        else:
            self._send("position startpos")
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
        deadline = time.time() + (movetime_ms / 1000.0) + 5.0
        assert self.proc.stdout is not None
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"{self.name}: engine closed mid-search")
            line = line.strip()
            if line.startswith("info "):
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
                return chess.Move.from_uci(tokens[1])
        raise TimeoutError(f"{self.name}: bestmove never arrived")

    def quit(self) -> None:
        try:
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
    board = opening.copy() if opening is not None else chess.Board()
    if status_ref is not None:
        status_ref["last_move"] = "—"
    if fen_log is not None:
        fen_log(f"start {white.name} vs {black.name}  fen {board.fen()}")
    if game_label:
        opening_str = " ".join(m.uci() for m in board.move_stack)
        if opening_str:
            print(f"{game_label} opening: {opening_str}", flush=True)
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
        movetime_ms = movetime_white if board.turn == chess.WHITE else movetime_black
        depth_cap = depth_white if board.turn == chess.WHITE else depth_black
        move = engine.go(board, movetime_ms, depth_cap)
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
                         " played from each side. 0 = pure startpos.")
    ap.add_argument("--seed", type=int, default=12345,
                    help="seed for random openings (reproducible matches)")
    ap.add_argument("--pgn", type=Path, default=None,
                    help="output PGN; default runs/match-<timestamp>.pgn")
    ap.add_argument("-j", "--jobs", nargs="?", type=int, const=-1, default=1,
                    help="parallel games (make-style). No flag = serial."
                         " -j alone = number of physical cores."
                         " -jN = N workers, capped at the physical core count"
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
    if args.jobs == -1:
        jobs = phys
    else:
        jobs = args.jobs
    jobs = max(1, min(jobs, phys, args.games))
    # One physical-core CPU id per worker for pinning. Empty list
    # disables pinning (e.g. /proc/cpuinfo unavailable, or --no-pin).
    pin_cpus: list[int] = []
    if not args.no_pin and jobs > 1:
        pin_cpus = physical_core_cpus()[:jobs]

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
    openings = [random_opening(args.random_plies, rng)
                for _ in range(pair_count)] if args.random_plies > 0 else None

    pin_desc = (f", pinned to CPUs {pin_cpus}" if pin_cpus
                else (", no pin" if jobs > 1 else ""))
    print(f"# {name1} vs {name2} — {args.games} games × {args.movetime}ms/move "
          f"(jobs={jobs}, physical cores={phys}{pin_desc})", flush=True)

    score: dict[str, float] = {name1: 0.0, name2: 0.0}
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
        e1: Optional[Engine] = Engine(args.engine1, name1, pin_cpu=pin)
        e2: Optional[Engine] = Engine(args.engine2, name2, pin_cpu=pin)
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
                    e1 = Engine(args.engine1, name1, pin_cpu=pin)
                    e2 = Engine(args.engine2, name2, pin_cpu=pin)
        finally:
            for e in (e1, e2):
                if e is not None:
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

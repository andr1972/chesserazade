#!/usr/bin/env python3
# Copyright (c) 2024-2026 Andrzej Borucki
# SPDX-License-Identifier: Apache-2.0

"""Engine round-robin tournament with sub-quadratic comparisons.

Two-phase plan:
  1. Rough sort (mergesort, N·log₂N comparisons, --rough-games each).
     Each comparison is a short head-to-head; the higher score wins.
     No statistics — purely score-based, fast.
  2. Adjacent verify: for each consecutive pair in the rough ranking,
     run a longer match (--precise-games), escalating to 2× / 5× when
     the 95% CI still includes zero so close-strength pairs get the
     extra games they need without wasting them on lopsided ones.

Final output is a ranking table with Elo + 95% CI relative to the
top engine and to the next-lower engine.

Time estimate (--estimate) prints phase-1 and phase-2 wall time for
the given engine count and movetime so you can plan overnight runs
before committing.

Usage:
    tools/tourney.py --movetime 1000 a b c d e
    tools/tourney.py --movetime 1000 --estimate -n 10
    tools/tourney.py --movetime 1000 --movetime1 100 --movetime2 1000 a b
"""

from __future__ import annotations

import argparse
import math
import re
import subprocess
import sys
import time
from functools import cmp_to_key
from pathlib import Path
from statistics import NormalDist
from typing import Callable, Optional

REPO = Path(__file__).resolve().parent.parent
MATCH_PY = REPO / "tools" / "match.py"

# Average plies/game observed in chesserazade self-play; used for
# wall-time estimates. Real matches vary 60-180 ply depending on
# engine strength and movetime, but 119 is a reasonable midpoint.
AVG_PLIES = 119


def fmt_duration(seconds: float) -> str:
    if seconds < 60:
        return f"{seconds:.0f}s"
    if seconds < 3600:
        return f"{seconds/60:.0f}m"
    h = int(seconds // 3600)
    m = int((seconds % 3600) // 60)
    return f"{h}h {m:02d}m"


def estimate_seconds_per_match(games: int, movetime_ms: int,
                               jobs: int, mt1: int, mt2: int) -> float:
    """Wall-clock seconds for one head-to-head match. Both engines
    contribute moves so the per-game cost is plies × avg(mt1, mt2)."""
    avg_mt = (mt1 + mt2) / 2.0
    per_game_s = AVG_PLIES * avg_mt / 1000.0
    return games * per_game_s / max(1, jobs)


def mergesort_compares(n: int) -> int:
    """Worst-case comparison count for mergesort on n items."""
    if n <= 1:
        return 0
    return math.ceil(n * math.log2(n) - n + 1)


def parse_match_output(stdout: str) -> tuple[float, float, int, int]:
    """Pull (score1, score2, draws, wins1) from match.py stdout."""
    pat = re.compile(
        r"# === final: \S+ ([\d.]+) - ([\d.]+) \S+"
        r"\s+\((\d+)W / (\d+)D / (\d+)L"
    )
    for line in stdout.splitlines():
        m = pat.search(line)
        if m:
            return (float(m.group(1)), float(m.group(2)),
                    int(m.group(4)), int(m.group(3)))
    raise RuntimeError(
        f"could not find '=== final' line in match output:\n"
        f"...{stdout[-400:]}"
    )


_GAME_LINE = re.compile(
    r"^\[(\d+)/(\d+)\]\s+(\S+)\s+-\s+(\S+):\s+(1-0|0-1|1/2-1/2)\b"
)

# ANSI colours for per-game progress dots: green when engine1 wins,
# red when it loses, default-foreground for draws. Emitted only on
# real TTYs so log-file captures stay clean.
_DOT_GREEN = "\033[32m.\033[0m"
_DOT_RED = "\033[31m.\033[0m"
_DOT_PLAIN = "."


def _lower_cmdline(a: str, b: str, order: list) -> str:
    """Return whichever of `a`, `b` came first in the user's
    command-line engine list. Used to anchor dot colouring so a
    pair like 'engine #3 vs engine #5' always reports from #3's
    perspective regardless of phase or rough-rank inversion."""
    return a if order.index(a) <= order.index(b) else b


def _dot_for(result: str, white: str, black: str, track: str,
             coloured: bool) -> str:
    """Colour for one game's progress dot, from `track`'s perspective."""
    if not coloured or result == "1/2-1/2":
        return _DOT_PLAIN
    won = (result == "1-0" and white == track) or \
          (result == "0-1" and black == track)
    return _DOT_GREEN if won else _DOT_RED


def run_match(eng1: str, eng2: str, *,
              games: int, movetime: int,
              mt1: Optional[int] = None, mt2: Optional[int] = None,
              name1: str, name2: str,
              jobs: Optional[int] = None,
              progress: bool = False,
              track: Optional[str] = None,
              extra_args: Optional[list[str]] = None
              ) -> tuple[float, float, int]:
    """Run match.py and return (score1, score2, draws). When
    `progress` is true, stream stdout and emit a '.' per completed
    game on stderr so the caller can show live progress without
    losing the parsed final summary."""
    cmd = [sys.executable, str(MATCH_PY),
           "--engine1", eng1, "--engine2", eng2,
           "--games", str(games),
           "--movetime", str(movetime),
           "--name1", name1, "--name2", name2]
    if mt1 is not None:
        cmd += ["--movetime1", str(mt1)]
    if mt2 is not None:
        cmd += ["--movetime2", str(mt2)]
    if jobs is None:
        # Bare `-j` triggers match.py's phys-1 default (its const=-1
        # path). Without any `-j` at all, match.py would default to
        # serial — rarely what the caller wants.
        cmd += ["-j"]
    else:
        cmd += ["-j", str(jobs)]
    if extra_args:
        cmd += extra_args
    if not progress:
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        s1, s2, draws, _wins1 = parse_match_output(res.stdout)
        return s1, s2, draws
    # Streaming mode: print '.' to stderr per game-completion line,
    # coloured from `track`'s perspective (green=track wins, red=loses,
    # plain=draw). `track` defaults to name1 but the caller may pin it
    # to a specific engine — typically the user's command-line first
    # engine — so colours stay consistent across rough/precise phases
    # regardless of how the rough ranking flips name1/name2.
    coloured = sys.stderr.isatty()
    track_name = track if track in (name1, name2) else name1
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    captured: list[str] = []
    assert proc.stdout is not None
    for line in proc.stdout:
        captured.append(line)
        m = _GAME_LINE.match(line)
        if m:
            white, black, result = m.group(3), m.group(4), m.group(5)
            sys.stderr.write(_dot_for(result, white, black, track_name,
                                      coloured))
            sys.stderr.flush()
    rc = proc.wait()
    sys.stderr.write("\n")
    sys.stderr.flush()
    if rc != 0:
        raise RuntimeError(
            f"match.py exited {rc}; tail:\n{''.join(captured[-10:])}")
    s1, s2, draws, _wins1 = parse_match_output("".join(captured))
    return s1, s2, draws


# Wilson-based Elo CI mirrors `match.format_elo`. Inlined here so the
# tourney has no Python-import dependency on match.py — match.py is
# invoked as a subprocess for actual gameplay.
def elo_interval(score: float, n: float, alpha: float = 0.05
                 ) -> tuple[float, float, float]:
    if n <= 0:
        return 0.0, -math.inf, math.inf

    def to_elo(p: float) -> float:
        if p <= 0.0: return -math.inf
        if p >= 1.0: return math.inf
        return -400.0 * math.log10(1.0 / p - 1.0)

    z = NormalDist().inv_cdf(1.0 - alpha / 2.0)
    p_hat = score / n
    denom = 1.0 + z * z / n
    center = (p_hat + z * z / (2.0 * n)) / denom
    half = (z * math.sqrt(p_hat * (1.0 - p_hat) / n
                          + z * z / (4.0 * n * n))) / denom
    p_lo = max(0.0, center - half)
    p_hi = min(1.0, center + half)
    return to_elo(p_hat), to_elo(p_lo * n / max(n, 1)), to_elo(p_hi * n / max(n, 1))


def fmt_elo_bound(val: float) -> str:
    if math.isinf(val):
        return "-inf" if val < 0 else "+inf"
    return f"{val:+.0f}"


_CI_LEVELS = (50, 75, 90, 95)


def fmt_multi_ci(score: float, n: int) -> str:
    """Elo point + CI at 50/75/90/95% — same format as match.py's
    final summary so a single-pair tourney row reads the same as a
    one-shot match.py invocation."""
    point = elo_interval(score, n)[0]
    head = f"{fmt_elo_bound(point)}" if not math.isinf(point) else ""
    parts = []
    for pct in _CI_LEVELS:
        alpha = 1.0 - pct / 100.0
        _, lo, hi = elo_interval(score, n, alpha=alpha)
        if math.isinf(point) and point > 0:
            parts.append(f"{pct}%>{fmt_elo_bound(lo)}")
        elif math.isinf(point) and point < 0:
            parts.append(f"{pct}%<{fmt_elo_bound(hi)}")
        else:
            parts.append(f"{pct}%:[{fmt_elo_bound(lo)},{fmt_elo_bound(hi)}]")
    return (head + " " + " ".join(parts)).strip() + f"  N={n}"


def _print_ranking(ranking: list, adj_results: list, tc_desc: str,
                   names_order: list) -> None:
    """Render the final ranking table. Each row shows Elo gap to the
    next-ranked engine with 50/75/90/95 % CIs, oriented so positive
    Elo means 'the lower-cmdline-index engine of the pair won'. Same
    rule as dot colours, so signs and colours stay consistent. Final
    row reads (last)."""
    print()
    print(f"# === tournament ranking ({tc_desc}) ===")
    if ranking:
        print(f"# winner: {ranking[0]}")
    print(f"{'rank':<4}  {'engine':<24}  Δ vs next (50/75/90/95% CI)")
    for i, name in enumerate(ranking):
        if i < len(adj_results):
            r = adj_results[i]
            anchor = _lower_cmdline(r["higher"], r["lower"], names_order)
            anchor_score = (r["score_higher"] if anchor == r["higher"]
                            else r["score_lower"])
            cell = fmt_multi_ci(anchor_score, r["n"])
        else:
            cell = "(last)"
        print(f"{i+1:<4}  {name:<24}  {cell}")


def estimate_mode(n: int, movetime: int, jobs: int,
                  rough_games: int, precise_games: int,
                  mt1: int, mt2: int) -> None:
    rough_n = mergesort_compares(n)
    adj_n = max(0, n - 1)
    sec_rough = rough_n * estimate_seconds_per_match(
        rough_games, movetime, jobs, mt1, mt2)
    sec_precise = adj_n * estimate_seconds_per_match(
        precise_games, movetime, jobs, mt1, mt2)
    print(f"engines: {n}  movetime: {movetime} ms  "
          f"jobs: {jobs}  avg plies: {AVG_PLIES}")
    if mt1 != movetime or mt2 != movetime:
        print(f"handicap: eng1={mt1}ms  eng2={mt2}ms")
    print(f"phase 1 (rough × {rough_games} games):  "
          f"{rough_n} matches → {fmt_duration(sec_rough)}")
    print(f"phase 2 (adjacent × {precise_games} games): "
          f"{adj_n} matches → {fmt_duration(sec_precise)}")
    print(f"total estimate: {fmt_duration(sec_rough + sec_precise)}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Two-phase engine tournament (mergesort rough + "
                    "adjacent precise verify).",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("engines", nargs="*",
                    help="engine commands or paths. Names default to"
                         " the binary stem; collisions get the parent"
                         " directory prepended (same logic as match.py).")
    ap.add_argument("--movetime", type=int, default=None,
                    help="ms per move for every engine. Required for"
                         " tournaments of 3+ engines. Mutually exclusive"
                         " with --movetime1/--movetime2.")
    ap.add_argument("--movetime1", type=int, default=None,
                    help="handicap mode: ms/move for engine1. Must be"
                         " paired with --movetime2; turns the run into"
                         " a 2-engine match (exactly two engines required,"
                         " no --movetime).")
    ap.add_argument("--movetime2", type=int, default=None,
                    help="handicap mode: ms/move for engine2 (see"
                         " --movetime1).")
    ap.add_argument("--rough-games", type=int, default=100,
                    help="games per rough comparison (default 100)")
    ap.add_argument("--precise-games", type=int, default=0,
                    help="games per adjacent verify. Default 0 disables"
                         " phase 2 entirely (rough ranking only). Pass"
                         " e.g. --precise-games 1000 to opt in.")
    ap.add_argument("--max-precise", type=int, default=5000,
                    help="cap when escalating undecided pairs in"
                         " phase 2. Set equal to --precise-games to"
                         " disable escalation. Ignored when phase 2"
                         " itself is disabled.")
    ap.add_argument("-j", "--jobs", type=int, default=None,
                    help="passed through to match.py")
    ap.add_argument("--estimate", action="store_true",
                    help="print phase-1 / phase-2 wall-time estimate"
                         " and exit; -n then sets the engine count")
    ap.add_argument("-n", type=int, default=None,
                    help="for --estimate only: number of engines")
    ap.add_argument("--quiet", action="store_true",
                    help="suppress per-match progress lines; only"
                         " the final ranking table is printed")
    args = ap.parse_args()

    # Time-control mode: either uniform --movetime or handicap with
    # --movetime1+--movetime2 (exactly 2 engines). Mixing the two
    # would be ambiguous, missing both leaves the run undefined.
    handicap = args.movetime1 is not None or args.movetime2 is not None
    if handicap and args.movetime is not None:
        print("--movetime is mutually exclusive with --movetime1/--movetime2",
              file=sys.stderr)
        return 2
    if handicap and (args.movetime1 is None or args.movetime2 is None):
        print("--movetime1 and --movetime2 must be given together",
              file=sys.stderr)
        return 2
    if not handicap and args.movetime is None:
        print("--movetime is required (or use --movetime1+--movetime2"
              " for a 2-engine handicap match)", file=sys.stderr)
        return 2

    if handicap:
        mt1, mt2 = args.movetime1, args.movetime2
        # Use mt1 as match.py's --movetime baseline; mt2 will be
        # passed as --movetime2 override. Wall-time estimate uses the
        # arithmetic mean (per-game cost is ply × avg(mt1, mt2)).
        movetime = mt1
    else:
        movetime = args.movetime
        mt1 = mt2 = movetime

    jobs_for_estimate = args.jobs if args.jobs is not None else 11

    if args.estimate:
        if args.n is None and not args.engines:
            print("--estimate needs -n N or an engine list", file=sys.stderr)
            return 2
        n = args.n if args.n is not None else len(args.engines)
        if handicap and n != 2:
            print("--movetime1/--movetime2 require exactly 2 engines",
                  file=sys.stderr)
            return 2
        estimate_mode(n, movetime, jobs_for_estimate,
                      args.rough_games, args.precise_games, mt1, mt2)
        return 0

    if handicap and len(args.engines) != 2:
        print(f"--movetime1/--movetime2 require exactly 2 engines"
              f" (got {len(args.engines)})", file=sys.stderr)
        return 2
    if len(args.engines) < 2:
        print("need at least 2 engines (or --estimate -n N)", file=sys.stderr)
        return 2

    # Engine names: derive from binary stem, then disambiguate
    # collisions by parent dir, mirroring match.py's logic but
    # extended to the multi-engine case.
    paths = [Path(e.split()[0]) for e in args.engines]
    stems = [p.stem or e for p, e in zip(paths, args.engines)]
    used: dict[str, int] = {}
    names: list[str] = []
    for stem, path in zip(stems, paths):
        if stems.count(stem) == 1:
            names.append(stem)
        else:
            parent = path.parent.name or "?"
            cand = f"{parent}/{stem}"
            if names.count(cand) > 0 or cand in used:
                used[cand] = used.get(cand, 0) + 1
                cand = f"{cand}#{used[cand]}"
            names.append(cand)

    say = (lambda *a, **k: None) if args.quiet else (
        lambda *a, **k: print(*a, **k, flush=True))

    eng_by_name = dict(zip(names, args.engines))

    tc_desc = (f"{mt1}/{mt2} ms (handicap)" if handicap
               else f"{movetime} ms/move")
    jobs_desc = (f"jobs={args.jobs}" if args.jobs is not None
                 else f"jobs=default (~phys-1, est. {jobs_for_estimate})")
    say(f"# tournament: {len(args.engines)} engines, "
        f"{tc_desc}, {jobs_desc}")
    say(f"# engines: {', '.join(names)}")

    # ---- Phase 1: mergesort rough ranking ---------------------------
    rough_n_est = mergesort_compares(len(args.engines))
    rough_eta = rough_n_est * estimate_seconds_per_match(
        args.rough_games, movetime, jobs_for_estimate, mt1, mt2)
    say(f"# phase 1: mergesort × {args.rough_games} games "
        f"(~{rough_n_est} matches, est. {fmt_duration(rough_eta)})")

    rough_compares: dict[tuple[str, str], tuple[float, float]] = {}

    def rough_compare(a: str, b: str) -> int:
        # Negative → a is stronger (sorts earlier in descending-by-strength).
        key = (a, b) if a < b else (b, a)
        if key in rough_compares:
            sA, sB = rough_compares[key]
            if a > b:
                sA, sB = sB, sA
        else:
            t0 = time.time()
            sA, sB, _drw = run_match(
                eng_by_name[a], eng_by_name[b],
                games=args.rough_games, movetime=movetime,
                mt1=mt1 if mt1 != movetime else None,
                mt2=mt2 if mt2 != movetime else None,
                name1=a, name2=b, jobs=args.jobs,
                progress=not args.quiet,
                track=_lower_cmdline(a, b, names))
            dt = time.time() - t0
            say(f"  rough {a} vs {b}: {sA:.1f} - {sB:.1f}  "
                f"({fmt_duration(dt)})")
            stored = (sA, sB) if (a, b) == key else (sB, sA)
            rough_compares[key] = stored
        return -1 if sA > sB else (1 if sA < sB else 0)

    t_phase1 = time.time()
    ranking = sorted(names, key=cmp_to_key(rough_compare))
    say(f"# phase 1 done in {fmt_duration(time.time() - t_phase1)}")
    say(f"# rough ranking: {' > '.join(ranking)}")

    # ---- Phase 2: adjacent verify with escalation -------------------
    # Opt-in: --precise-games 0 (default) skips phase 2 entirely so a
    # bare invocation gives only the rough ordering. Pass an explicit
    # game count to enable.
    if args.precise_games <= 0:
        # Pull score data from the rough comparisons we already
        # made — mergesort guarantees every rank-adjacent pair was
        # compared during a merge, so each adjacency has a (sA, sB)
        # tuple in rough_compares. Use that for Elo with multi-CI;
        # the CIs will be wide because N is only --rough-games.
        adj_results: list[dict] = []
        for i in range(len(ranking) - 1):
            higher, lower = ranking[i], ranking[i + 1]
            key = (higher, lower) if higher < lower else (lower, higher)
            if key in rough_compares:
                sA, sB = rough_compares[key]
                if (higher, lower) == key:
                    s_h, s_l = sA, sB
                else:
                    s_h, s_l = sB, sA
                adj_results.append({
                    "higher": higher, "lower": lower,
                    "score_higher": s_h, "score_lower": s_l,
                    "n": args.rough_games,
                })
        say("# phase 2 skipped (--precise-games not set;"
            " pass e.g. --precise-games 1000 to enable)")
        _print_ranking(ranking, adj_results, tc_desc, names)
        return 0

    adj_count = max(0, len(ranking) - 1)
    adj_eta = adj_count * estimate_seconds_per_match(
        args.precise_games, movetime, jobs_for_estimate, mt1, mt2)
    say(f"# phase 2: {adj_count} adjacent pairs × "
        f"{args.precise_games} games (est. {fmt_duration(adj_eta)})")

    # Per adjacent pair: store cumulative (score_higher, score_lower, n_games)
    adj_results: list[dict] = []
    t_phase2 = time.time()
    # Granularity for intermediate progress reports — same as the
    # rough-phase block so the readout pace stays consistent across
    # phases and the rough seed shows up as the very first data point.
    chunk_size = args.rough_games
    for i in range(len(ranking) - 1):
        higher, lower = ranking[i], ranking[i + 1]
        # `anchor` = lower-cmdline-index engine of the pair, same
        # rule as the dot colours. Elo + score in this row's display
        # is from anchor's perspective, so signs and colours stay
        # consistent: green dot ↔ anchor wins ↔ Elo positive.
        anchor = _lower_cmdline(higher, lower, names)
        # Reuse the rough-phase score for this pair as a down-payment
        # against the precise target. mergesort guarantees adjacency
        # implies a direct comparison was made; if that pair somehow
        # wasn't measured (defensive — shouldn't happen), start fresh.
        rough_key = (higher, lower) if higher < lower else (lower, higher)
        if rough_key in rough_compares:
            sA, sB = rough_compares[rough_key]
            if (higher, lower) == rough_key:
                cum_h, cum_l = sA, sB
            else:
                cum_h, cum_l = sB, sA
            played = args.rough_games
            anchor_s = cum_h if anchor == higher else cum_l
            say(f"  adj  {higher} vs {lower}  (seed from rough)")
            say(f"    N={played:<5} {cum_h:.1f}-{cum_l:.1f}  "
                f"{fmt_multi_ci(anchor_s, played)}")
        else:
            cum_h = cum_l = 0.0
            played = 0
            say(f"  adj  {higher} vs {lower}")

        def play_chunk(n: int) -> None:
            nonlocal cum_h, cum_l, played
            t0 = time.time()
            sH, sL, _drw = run_match(
                eng_by_name[higher], eng_by_name[lower],
                games=n, movetime=movetime,
                mt1=mt1 if mt1 != movetime else None,
                mt2=mt2 if mt2 != movetime else None,
                name1=higher, name2=lower, jobs=args.jobs,
                progress=not args.quiet,
                track=anchor)
            cum_h += sH; cum_l += sL; played += n
            anchor_s = cum_h if anchor == higher else cum_l
            say(f"    N={played:<5} {cum_h:.1f}-{cum_l:.1f}  "
                f"{fmt_multi_ci(anchor_s, played)}  "
                f"(+{n} in {fmt_duration(time.time()-t0)})")

        def is_decisive() -> bool:
            anchor_s = cum_h if anchor == higher else cum_l
            _, lo, hi = elo_interval(anchor_s, played)
            return lo > 0 or hi < 0

        # Run chunks until the precise target is met. Then, if the
        # 95% CI still straddles 0 and --max-precise allows it,
        # keep going up to the cap.
        while played < args.precise_games:
            play_chunk(min(chunk_size, args.precise_games - played))
        while (played < args.max_precise
               and not is_decisive()):
            play_chunk(min(chunk_size, args.max_precise - played))

        adj_results.append({
            "higher": higher, "lower": lower,
            "score_higher": cum_h, "score_lower": cum_l,
            "n": played,
        })
    say(f"# phase 2 done in {fmt_duration(time.time() - t_phase2)}")

    _print_ranking(ranking, adj_results, tc_desc, names)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
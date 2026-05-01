# chesserazade

An **educational** chess program written in modern C++23. Its
primary value is that its source code is a clean, classical
reference someone can *read and learn from* — not its playing
strength.

It is **not** a competitive engine. It will not out-play
Stockfish, Leela, or any modern engine. That is a non-goal.

What it **is** good at:

- **Legal move generation + perft**: passes the six standard
  perft positions at depth 5 (plus the initial position at
  depth 6, verified by the `perft` CLI).
- **Puzzle solving**: finds forced mate-in-N from a FEN.
- **Game analysis**: annotates a PGN with the engine's verdicts
  (eval, best move, NAG glyphs `?`, `??`, `?!`).
- **Interactive play**: a minimal REPL you can drive by typing
  moves in UCI or SAN.
- **Pulling games / puzzles from the internet** via an explicit,
  user-confirmed `fetch` command, then feeding them into
  `analyze` or the REPL.

---

## Build

### Requirements

- **C++23** compiler (GCC ≥ 13 or Clang ≥ 17; tested on GCC).
- **CMake ≥ 3.25** and **Ninja** (the generator in
  `CMakePresets.json`).
- **curl** on `PATH` (only for the `fetch` subcommand).
- No other third-party libraries. Catch2 is pulled via
  `FetchContent` on first test build.

### Configure + build

```sh
cmake --preset release-with-asserts
cmake --build build/release-with-asserts
```

Three build presets are available:

| Preset                 | What it's for                                           |
| ---------------------- | ------------------------------------------------------- |
| `debug`                | Asserts on, no optimization — the default for dev work. |
| `release-with-asserts` | `-O2 -g -UNDEBUG` — recommended for benchmarks / puzzles. |
| `release`              | `-O3`, asserts off — headline speed, no debug info.     |

### Run the tests

```sh
ctest --preset release-with-asserts
```

All tests must pass; any failure is a regression, not a flake.
The perft suite dominates the runtime — ~25 s in
`release-with-asserts`, ~170 s in `debug`.

---

## Quick start

```sh
# Show the starting position
chesserazade show

# Render an arbitrary FEN (ASCII; add --unicode for figurines)
chesserazade show --fen "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"

# Count leaf nodes to depth 4 from the starting position
chesserazade perft --depth 4             # → 197281

# List legal moves (UCI) of the current side to move
chesserazade moves --fen "<fen>"

# Find the best move at depth 6
chesserazade solve --fen "<fen>" --depth 6

# Solve a mate-in-3 puzzle
chesserazade solve --fen "<fen>" --mate-in 3

# Interactive session — enter moves in UCI or SAN, undo, save PGN
chesserazade repl

# Annotate a PGN file
chesserazade analyze --pgn game.pgn --depth 8

# Pull a PGN from the internet (interactive, asks for
# confirmation before the request is sent)
chesserazade fetch
```

---

## Engine matches and tournaments

Three Python helpers under `tools/` drive head-to-head play and
post-match analysis. All need `python-chess` — activate a venv
that has it before running.

```bash
# Single head-to-head match (40 games × 1000 ms/move, 11 workers)
python tools/match.py --engine1 ./build/release/chesserazade \
                      --engine2 ./path/to/rukh \
                      --games 40 --movetime 1000 -j

# Two-phase tournament: mergesort rough ranking, then precise
# verify between adjacent ranks. Sub-quadratic in the number of
# engines so 10-15 entrants is feasible overnight.
python tools/tourney.py --movetime 1000 \
       ./build/release/a ./build/release/b /path/to/rukh stockfish

# Wall-time estimate before committing to an overnight run
python tools/tourney.py --estimate --movetime 1000 -n 10

# Handicap match — measure Elo per time-doubling
# (--movetime1/--movetime2 replace --movetime; exactly two engines)
python tools/tourney.py --movetime1 100 --movetime2 1000 a b

# Quiet mode — only the final ranking table is printed
python tools/tourney.py --quiet --movetime 1000 a b c

# Blunder hunt — diff a candidate build against a reference, find
# where they disagreed during a lost game, and rank both moves with
# Stockfish so it's clear which choice was the actual blunder.
python tools/blunder_hunt.py \
       --candidate ./build/release/chesserazade \
       --reference ./build/release/a \
       --movetime 1000
```

`tourney.py` defaults: 100 games per rough comparison, 1000 per
adjacent verify with escalation to 5000 when the 95 % Elo CI still
includes zero. All parameters are overridable; see `--help`.

### Opening books for fairer matches

The `--random-plies` opening generator is convenient but has bias —
the same `--seed` always produces the same first openings, so a
multi-chunk phase-2 verify can re-test the same positions instead
of sampling new ones. For longer matches a curated opening book is
the standard. We don't ship one in-tree (they're large and
maintained externally); download into `tools/openings/` (gitignored)
and point match.py at it:

```bash
mkdir -p tools/openings
cd tools/openings

# noob_3moves: ~11 000 positions after 3 fullmoves (= 6 ply).
# Balanced, the usual recommendation for engines below SF strength.
wget https://github.com/official-stockfish/books/raw/master/noob_3moves.epd.zip
unzip noob_3moves.epd.zip

# Or 8moves_v3 (deeper, ~7 000 positions after 16 ply) if the
# engine handles middlegame structure well.
wget https://github.com/official-stockfish/books/raw/master/8moves_v3.epd.zip
unzip 8moves_v3.epd.zip
```

(Hooking these into match.py / tourney.py via an `--openings` flag
is a future change; today the random-plies generator with a unique
per-chunk seed is what's wired in.)

---

## Architecture at a glance

```
  CLI                   src/cli/cmd_*.cpp — one file per subcommand
   │
   ▼
  Game + PGN / SAN      include/chesserazade/{game,pgn,san}.hpp
   │
   ▼
  Search / Evaluator    include/chesserazade/{search,evaluator,
  + TT + PuzzleSolver   transposition_table,puzzle_solver,
  + GameAnalyzer        game_analyzer}.hpp
   │
   ▼
  Move generator        include/chesserazade/move_generator.hpp
   │
   ▼
  Board (interface)     include/chesserazade/board.hpp
  │                     + src/board/board8x8_mailbox.{hpp,cpp}
   ▼
  Core types + FEN I/O  include/chesserazade/{types,fen,move}.hpp
```

For the full map of what lives where — layered diagram, data
flow, design invariants, extension points — see
**`docs/architecture.md`** (EN) / **`docs/architecture_pl.md`**
(PL).

Per-version change log:
- `docs/version_notes/0.1.md` … `1.0.md` (each with a `_pl`
  Polish twin).

Implementation plan and design rationale:
**`HANDOFF.md`** / `docs/HANDOFF_pl.md`.

---

## Known limitations

These are deliberate scope choices, not bugs. They are
summarized here so a reader of the source does not go hunting
for them.

- **Two board implementations.** `Board8x8Mailbox` is the
  classical mailbox reference; `BoardBitboard` (1.1) is the
  bitboard alternative (~2.5–3× faster on perft). The CLI
  `perft --board mailbox|bitboard` picks between them. The
  slider attack helpers use a loop-based ray walk; magic /
  PEXT bitboards are an optional post-1.1 polish.
- **No opening book, no endgame tablebase.** The engine plays
  every position from search + evaluator.
- **No UCI-protocol adapter.** You cannot drop chesserazade
  into Arena / CuteChess today. A UCI shim is on the post-1.2
  roadmap.
- **Evaluator is material + piece-square tables.** No king
  safety phase interpolation, no pawn structure terms, no
  mobility term beyond what the PST provides. Strength stops
  somewhere around 1400–1600 Elo.
- **Quiescence searches captures only** — no checks, no
  check-evasion quiescence. Tactical depth is honest but
  shallow.
- **Move ordering uses TT move + MVV-LVA + killers.** The
  history heuristic is not implemented.
- **PGN writer drops comments from the input.** Parser accepts
  `{…}` / `;…` comments and skips `(…)` variations, but the
  writer does not reproduce either. Round-trip of tags + moves
  + termination is exact; round-trip of commentary is not.
- **Positive NAGs (`!`, `!!`) are not emitted.** The analyzer
  only flags mistakes and blunders.
- **`fetch` shells out to `curl`.** No libcurl dependency; but
  this means `curl` must be installed.
- **Single-threaded.** No SMP search.
- **No neural networks, no NNUE.** Classical evaluator only —
  explicitly out of scope for this project.

---

## License

See `LICENSE`.

## Further reading

The Chess Programming Wiki (`https://www.chessprogramming.org`)
is the reference this codebase draws from. Module doc-blocks
link directly to the relevant page where applicable.

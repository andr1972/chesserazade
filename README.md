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

- **Mailbox board only.** The `Board` interface is abstract;
  `Board8x8Mailbox` is the sole concrete implementation. A
  bitboard implementation is on the 1.1 roadmap; the mailbox
  will stay in the tree as the reference.
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

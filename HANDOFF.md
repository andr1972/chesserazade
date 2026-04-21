# Chesserazade — Implementation Handoff

**Audience:** a software engineer (or coding agent) who will implement this project from scratch, version by version.
**Repository:** `chesserazade` (currently empty except for `README.md`, `LICENSE`, `.gitignore`).
**Language of communication with the user:** Polish. Language of code, comments, identifiers, commit messages, CLI output: English.

---

## 1. Purpose & Philosophy

Chesserazade is an **educational chess program**. Its primary value is that its source code is a clean, classical reference someone can *read and learn from* — not its playing strength.

Design north stars, in order:

1. **Readable source code.** Someone opening any `.cpp` / `.hpp` file should understand what it does, why, and how it fits the whole. Named types, named constants, descriptive functions, and deliberate comments where the *why* is non-obvious. This project intentionally overrides the usual "no comments" default: thoughtful doc-comments on public APIs and non-trivial algorithms are *expected*.
2. **Classical approach.** Use the techniques that a textbook on chess programming would describe: mailbox board representation, pseudo-legal + legality check move generation, make/unmake, minimax, alpha-beta pruning, Zobrist hashing, move ordering, quiescence search. No neural networks, no NNUE, no exotic SIMD tricks in the 0.x–1.0 line.
3. **Modularity.** Major subsystems (board, move generator, evaluator, searcher, protocol / CLI front-end, puzzle solver, PGN I/O) are isolated behind well-named interfaces so that a reader can study them independently, and so that in 1.1 the mailbox board can be swapped for a bitboard board without touching the rest.
4. **Correctness over speed.** Pass the standard perft positions exactly. Optimizations happen only *after* correctness.

### What this project is **not**

- Not a competitive engine. It will not out-play Stockfish, Leela, Komodo, or any modern engine. That is explicitly a non-goal.
- Not primarily a tool for humans to play chess against. Human-vs-human and human-vs-computer gameplay are secondary.
- Not a research platform for novel algorithms.

### What this project **is used for**

Primary use cases, in order of importance:

1. **Legal move generation & perft** — generate every legal move to a given depth, count nodes, measure time. Usable both as a CLI tool and as a library.
2. **Puzzle solving** — given a FEN, find mate-in-N (N ≥ 1), find tactical motifs, find best move at a given search depth.
3. **Game analysis** — given a PGN, annotate each move with the engine's evaluation and best-line suggestion; highlight blunders and missed tactics.
4. **Fetching puzzles and historical games from the internet** — e.g. Fischer's games, FIDE composition problems, Lichess puzzles. This is **interactive**: the program asks the user which source/player/problem set to pull, downloads it, and loads it for analysis.
5. **Human-vs-human and human-vs-computer play** — a minimal console interface exists, but is a side-effect of the features above, not the headline feature.

---

## 2. Target Audience for the Source Code

A reader of the source should be:

- A programmer who knows C++ at an intermediate level but is new to chess programming, or
- A chess programmer who wants to see a clean, modern-C++ reference implementation of classical techniques.

Write for that reader. If you implement alpha-beta, a short comment block at the top of the function should remind the reader what alpha-beta is and why the bounds work. If you implement Zobrist hashing, explain in a few sentences what it hashes and why collisions are acceptable. Link to Chess Programming Wiki (`https://www.chessprogramming.org`) from module headers where relevant.

---

## 3. Technical Requirements

- **Language:** C++23 preferred, C++20 minimum. Use C++23 features (`std::expected`, `std::print`, `std::format`, `std::mdspan` if useful, `if consteval`, improved ranges) where they make the code clearer. Do **not** use C++23 features merely to show off.
- **Build:** CMake ≥ 3.25. Presets (`CMakePresets.json`) for `debug`, `release`, `release-with-asserts`.
- **Compilers:** must build cleanly on GCC ≥ 13 and Clang ≥ 17 under Linux. Warnings-as-errors (`-Wall -Wextra -Wpedantic -Werror`).
- **Dependencies:** standard library only for the core. `Catch2` via `FetchContent` for tests. Nothing else for 0.x–1.0. `{fmt}` is tempting but not needed if C++23 `std::print`/`std::format` is available. No Boost.
- **Platforms:** Linux is the first-class target. macOS should work. Windows is best-effort.
- **Output:** a single console executable `chesserazade`. A separate static library target `chesserazade_core` holds the engine logic, so tests and `chesserazade` both link against it.

### Coding style (binding)

- **Templates:** use only where they arise naturally from DRY — e.g. a small numeric utility that genuinely works for multiple integer widths. Do **not** template-ify `Board`, `MoveGenerator`, `Search`. Polymorphism, where needed, uses abstract base classes and `virtual`, because the program is didactic and a vtable is a tiny, well-understood cost.
- **Lambdas:** avoid in general. A named function is easier to read and to step through in a debugger. `string_view`, `span`, `optional`, `expected`, `variant`, `array`, `bitset` are all fine and encouraged where they clarify intent.
- **No macros** except the include guard equivalents (prefer `#pragma once`). No `using namespace std;` at file scope.
- **Error handling:** `std::expected<T, Error>` for recoverable errors (parse failures, malformed FEN, illegal move input). `assert` for invariants that indicate engine bugs. No exceptions on hot paths; exceptions allowed only at the CLI/front-end boundary.
- **Naming:** types `PascalCase`, functions and variables `snake_case`, constants `SCREAMING_SNAKE_CASE`, enum values `PascalCase`. Headers `.hpp`, sources `.cpp`. One public type per header where practical.
- **Headers:** every public header starts with a `///` doc-block describing the module's purpose, its invariants, and a link to the relevant Chess Programming Wiki page if one exists.
- **Files ≤ ~400 lines.** If a file grows beyond that, it's a signal to split by concept, not by line count.

---

## 4. Architecture Overview

```
+-----------------------------------------------------------+
|                        CLI / main                         |
|  command dispatch, argument parsing, interactive prompts  |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|                   Application services                    |
|  perft runner | puzzle solver | game analyzer | net fetch |
+-------+---------------+---------------+-------------------+
        |               |               |
+-------v------+ +------v------+ +------v---------+
|   Search     | | Evaluator   | |   PGN / FEN    |
|  minimax,    | | material,   | |   parsing &    |
|  alpha-beta, | | piece-sq    | |   serialization|
|  TT, ordering| | tables      | |                |
+-------+------+ +------+------+ +----------------+
        |               |
+-------v---------------v-----------------------------------+
|                    Move Generator                         |
|    pseudo-legal generation, legality filtering,           |
|    make/unmake, Zobrist hashing                           |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|                       Board (interface)                   |
|     Board8x8Mailbox (1.0)   |   BoardBitboard (1.1)       |
+-----------------------------------------------------------+
```

### Key abstractions

- **`Board`** — abstract interface: query piece at square, side to move, castling rights, en-passant square, halfmove clock, fullmove number, Zobrist key; `make_move` and `unmake_move`; equality by position (not by history). The interface is small and stable so that 1.1 can introduce `BoardBitboard` without changes elsewhere.
- **`Move`** — a compact POD: from-square, to-square, moved piece, captured piece, promotion piece, flags (castle, en passant, double push). 16 bits if packed, but clarity first — a 32- or 64-bit struct is fine.
- **`MoveGenerator`** — free functions or a stateless class; takes a `const Board&`, returns a `MoveList` (small-vector, fixed capacity 256 is safe for chess). Provides `generate_legal(board)` and `generate_pseudo_legal(board)`.
- **`Evaluator`** — takes a `const Board&`, returns a score in centipawns from the side-to-move's perspective.
- **`Search`** — takes a `Board`, a limit (depth, nodes, or time), an `Evaluator`, and returns a `SearchResult { best_move, score, principal_variation, nodes, elapsed }`.
- **`Pgn`, `Fen`** — pure parsing/serialization, no engine state.
- **`PuzzleSolver`** — orchestrates `Search` to find mate-in-N or best tactical move; prints solutions with human-readable PV.
- **`GameAnalyzer`** — runs `Search` at each ply of a PGN game and reports evaluation deltas and suggested alternatives.
- **`NetFetcher`** — a small service (behind an interface) that pulls puzzles/games from a configured source. Interactive: it prompts the user to choose source and query.

---

## 5. Directory Layout

```
chesserazade/
├── CMakeLists.txt
├── CMakePresets.json
├── HANDOFF.md                  (this file)
├── README.md
├── LICENSE
├── docs/
│   ├── HANDOFF_pl.md           (Polish translation of this file)
│   ├── architecture.md         (written in 0.2, updated each version)
│   ├── coding_style.md         (written in 0.1)
│   └── version_notes/
│       ├── 0.1.md
│       ├── 0.2.md
│       └── ...
├── include/chesserazade/       (public headers used by tests and main)
│   ├── board.hpp
│   ├── move.hpp
│   ├── move_generator.hpp
│   ├── fen.hpp
│   ├── pgn.hpp                 (from 0.4)
│   ├── evaluator.hpp           (from 0.5)
│   ├── search.hpp              (from 0.5)
│   ├── zobrist.hpp             (from 0.7)
│   ├── transposition_table.hpp (from 0.7)
│   ├── puzzle_solver.hpp       (from 0.8)
│   ├── game_analyzer.hpp       (from 0.9)
│   └── net_fetcher.hpp         (from 1.0)
├── src/
│   ├── main.cpp
│   ├── cli/
│   │   ├── command_dispatch.cpp
│   │   ├── cmd_perft.cpp
│   │   ├── cmd_solve.cpp
│   │   ├── cmd_analyze.cpp
│   │   └── ...
│   ├── board/
│   │   ├── board8x8_mailbox.cpp
│   │   └── board8x8_mailbox.hpp
│   ├── move_generator/
│   ├── search/
│   ├── eval/
│   ├── io/
│   └── net/
├── tests/
│   ├── CMakeLists.txt
│   ├── test_fen.cpp
│   ├── test_move_generator.cpp
│   ├── test_perft.cpp
│   ├── test_search.cpp
│   ├── test_pgn.cpp
│   └── data/
│       ├── perft_positions.txt
│       ├── puzzles_mate_in_2.txt
│       └── sample_games.pgn
└── tools/
    └── perft_bench.cpp         (optional stand-alone perft binary)
```

---

## 6. CLI Surface

The executable `chesserazade` is invoked with a sub-command. Every sub-command prints `--help`. Examples:

```
chesserazade perft --depth 5 --fen <fen>
chesserazade perft --depth 5 --divide                 # prints per-move counts
chesserazade moves --fen <fen>                        # list legal moves in position
chesserazade show --fen <fen>                         # pretty-print the board
chesserazade solve --fen <fen> --mate-in 2
chesserazade solve --fen <fen> --depth 8
chesserazade analyze --pgn game.pgn --depth 12
chesserazade fetch                                    # interactive prompt
chesserazade play                                     # minimal interactive play (from 0.4)
chesserazade repl                                     # interactive shell w/ all commands (from 0.4)
chesserazade version
```

Rules:

- Every sub-command is implemented in its own file under `src/cli/`.
- Input format for positions is FEN. For moves, accept UCI (`e2e4`, `e7e8q`) and optionally SAN (`Nf3`) from 0.4.
- Every sub-command that may take time prints a final summary line: `nodes N, time T ms, N/s`.

---

## 7. Testing Strategy

- **Catch2** pulled via `FetchContent`.
- **Perft correctness is non-negotiable.** The move generator must pass the six standard perft positions (see `https://www.chessprogramming.org/Perft_Results`) to depth ≥ 5 (initial position to depth 6). These become CI tests from 0.2 onward.
- **FEN round-trip tests:** parse → serialize → parse must be stable.
- **PGN round-trip tests** from 0.4.
- **Search regression tests:** a handful of well-known mate-in-2, mate-in-3, and tactics puzzles with known answers, from 0.5 onward.
- **No flaky tests.** Time-limited tests are forbidden in CI; depth-limited or node-limited only.
- **Benchmarks are not tests.** A `tools/perft_bench.cpp` may exist but is not run in CI.

---

## 8. Internet Fetching (1.0)

The `fetch` subcommand is interactive. Expected flow:

1. Program prints a menu of available sources (initially: Lichess puzzles API, a chess-games repository such as `pgnmentor.com` or a local mirror, a user-supplied URL).
2. User picks a source.
3. Program prompts for a query (player name, theme, puzzle rating range, date range).
4. Program downloads the data, caches it under `~/.cache/chesserazade/`, and offers to load it for analysis.

Design constraints:

- **Isolate network code behind a `NetFetcher` interface** so the rest of the program (and tests) never touches the network directly. Tests inject a fake fetcher.
- **Respect rate limits and terms of service** of any external API.
- **All network calls are explicit, logged, and require user confirmation** before being sent. No background traffic.
- **Cache aggressively.** A puzzle set or game archive pulled once should not be re-downloaded.

The implementing agent is expected to confirm with the user which concrete sources to support in 1.0; Lichess puzzles + one historical-games source is a reasonable minimum.

---

## 9. Version Roadmap

Each version ends in a signed git tag (`v0.1`, `v0.2`, …). Each version produces a `docs/version_notes/<version>.md` summarizing what was added, what tests pass, and what is known to be missing.

### 0.1 — Foundations
- `Board` interface + `Board8x8Mailbox` implementation (no en passant, no castling yet — just piece placement, side to move).
- FEN parsing and serialization.
- `show --fen` command.
- `Move` type and a move printer (UCI notation).
- Project scaffolding: CMake, presets, `.clang-format`, CI stub, Catch2 wired up.
- **Acceptance:** `chesserazade show --fen "<standard initial position FEN>"` renders the board. FEN round-trip tests pass.

### 0.2 — Full legal move generation
- Pseudo-legal generation for all pieces.
- Legality filtering (king not in check after move).
- **All special moves:** castling (both sides, both colors, with correct rights and through-check rules), en passant, promotions (all four pieces).
- `make_move` / `unmake_move` with full state restore.
- `moves --fen <fen>` command.
- `perft --depth N --fen <fen>` command with `--divide` option and timing output.
- **Acceptance:** passes the six standard perft positions to depth 5 (initial to depth 6). CI enforces.

### 0.3 — Hardening the generator
- Perft test suite wired into CI.
- Micro-benchmarks in `tools/perft_bench.cpp` for the implementer's own feedback (not run in CI).
- Documentation pass: every public header has a doc-block; `docs/architecture.md` drafted.
- **Acceptance:** perft on standard positions to depth 6 completes in reasonable time (a single-digit minutes is acceptable for a mailbox implementation — speed is not the point yet).

### 0.4 — PGN, history, minimal interactive play
- PGN parser and writer (full tag pairs, SAN moves, comments, variations optional — at minimum main-line SAN).
- Move history stack in an `Game` class sitting above `Board`.
- `repl` / `play` subcommand: tiny text UI to enter moves (UCI and SAN), undo, show, save PGN.
- **Acceptance:** can load a sample PGN, step through it, save it back, and the diff is semantically equivalent.

### 0.5 — Material evaluation + minimax
- `Evaluator` with piece values and piece-square tables (classical values from Chess Programming Wiki).
- `Search` with plain minimax (negamax), fixed depth.
- `solve --fen <fen> --depth N` subcommand.
- **Acceptance:** finds forced mate-in-1 and mate-in-2 on standard test positions.

### 0.6 — Alpha-beta + iterative deepening + time control
- Alpha-beta pruning replaces plain minimax.
- Iterative deepening.
- Time limit (`--time-ms`) and node limit (`--nodes`).
- Principal variation output.
- **Acceptance:** solves mate-in-3 on standard test positions; PV is displayed; search respects time limits within ~5%.

### 0.7 — Zobrist hashing + transposition table
- Zobrist keys maintained incrementally in `make_move` / `unmake_move`.
- Fixed-size transposition table with replacement scheme (age + depth).
- TT probes and stores in `Search`.
- This addresses the user's note about "cechowanie ruchów A,B,C = C,B,A" — a Zobrist-keyed TT detects move-order-independent transpositions.
- **Acceptance:** measurable node reduction on tactical positions; TT hit rate logged.

### 0.8 — Move ordering + quiescence search + puzzle solver
- Move ordering: TT move, MVV-LVA for captures, killer moves, history heuristic.
- Quiescence search for captures (and optionally checks).
- `PuzzleSolver` with `--mate-in N` option: uses negamax with mate-scoring.
- **Acceptance:** solves a curated set of mate-in-2 and mate-in-3 puzzles with correct first move and full PV.

### 0.9 — Game analyzer
- `analyze --pgn file.pgn --depth N`: for each position, report engine evaluation, best move, and tag blunders (eval swing > threshold).
- Output format: annotated PGN with NAG glyphs (`?`, `??`, `!`, `!!`, `?!`, `!?`) and PV comments.
- **Acceptance:** produces a sensible annotated PGN for a known blunder-ridden sample game.

### 1.0 — Internet fetching, polish, documentation
- `fetch` subcommand as described in §8.
- Full pass on documentation: `docs/architecture.md` reflects the final state; every module has its doc-block.
- `README.md` rewritten with build instructions, feature list, and a quick-start.
- Known-limitations section in `README.md` and in each module.
- Version tagged `v1.0`.
- **Acceptance:** user can pull Fischer's games via `fetch`, run `analyze` on one of them, and read the result.

### 1.1 — Bitboard alternative
- `BoardBitboard` implementation of the `Board` interface.
- Magic bitboards (or PEXT where available) for sliding pieces. This is the one place where some complex constants and table-generation code is expected.
- A runtime flag `--board=mailbox|bitboard` (or a CMake option) picks which implementation the executable uses. Both pass the same perft tests. The mailbox implementation **stays** in the tree as the reference.
- **Acceptance:** `perft` with bitboard passes the same correctness suite and is measurably faster than mailbox.

### 1.2 — Qt6 GUI
- A Qt6 front-end in a separate CMake target `chesserazade_gui`, linking against `chesserazade_core`.
- Board display, drag-and-drop moves, PGN load/save, invoke the searcher and puzzle solver, display evaluation bar and PV.
- **Acceptance:** the existing console executable is unaffected; the GUI is a pure additional front-end.

### Post-1.2 TODO (not planned in detail)
- **UCI protocol** — so the engine can be plugged into Arena, CuteChess, SCID, etc. Specification: `https://wbec-ridderkerk.nl/html/UCIProtocol.html`.
- **Opening book** (Polyglot format) as an optional read-only source.
- **Endgame tablebase probing** (Syzygy) as an optional read-only source.
- **SMP (multi-threaded) search**, e.g. Lazy SMP.
- **NNUE** is explicitly out of scope — it would undermine the "classical, educational" identity of the project.

---

## 10. Workflow & Deliverables Between Versions

- **One feature branch per version.** Merge with a squash or a clean merge commit to `main`. Tag the result.
- **Each version PR must:**
  - list what was added,
  - link to the relevant `docs/version_notes/<version>.md`,
  - show that all pre-existing tests still pass,
  - show the new tests introduced for this version.
- **Commit messages:** imperative mood, scoped (`move-gen: add en-passant generation`), bodies explain *why* when non-obvious.
- **No commented-out code** left in `main`. Delete it; git remembers.
- **No TODO/FIXME** in `main` without an associated issue/ticket or note in `docs/version_notes/`.

---

## 11. Open Questions for the Implementing Agent

Resolve these with the user before starting the corresponding version:

1. **0.4 PGN scope:** variations and NAGs in 0.4 or deferred to 0.9? (Recommendation: parse and preserve them, ignore them in 0.4 analysis; use them in 0.9.)
2. **0.9 analysis output format:** annotated PGN only, or also a plain-text report? (Recommendation: both, plain text default, `--pgn-out` to write annotated PGN.)
3. **1.0 fetch sources:** which concrete APIs/sites? Lichess puzzle API is an obvious start; confirm which historical-games source (pgnmentor.com, chessgames.com archives, Lichess study exports, or a local PGN directory) the user wants.
4. **SAN parsing strictness:** reject ambiguous SAN? (Recommendation: yes, with a clear error message.)
5. **Character encoding of console output:** use Unicode figurine pieces (`♔♕♖♗♘♙`) by default? Fall back to ASCII (`KQRBNP`) on `--ascii`? (Recommendation: yes, Unicode default with ASCII fallback.)

---

## 12. Translation

After finalizing this document, create `docs/HANDOFF_pl.md` — a faithful Polish translation. Keep the file structure, headings, and technical terms (Polish text, English identifiers and tool names). Keep the two files in sync on every subsequent edit.

---

*End of handoff. When in doubt, optimize for the reader of the source code, not for the benchmark.*

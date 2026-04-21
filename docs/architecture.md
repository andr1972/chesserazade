# Chesserazade — Architecture

Draft: 0.8 (reflects code through `v0.8.0`). Updated every version.

This document is for the reader who has `git clone`d the tree and
wants a map before diving into the source. It complements
`HANDOFF.md` (which is the implementation plan) by describing what
**actually exists today**, how the pieces fit together, and where the
0.4+ subsystems will plug in.

---

## 1. Shape of the program

At the top level there is **one static library** and **two
executables**:

- `chesserazade_core` — the engine. Every piece of chess logic
  (types, board, FEN, move generator) lives here. No I/O beyond
  returning strings.
- `chesserazade` — the CLI. Links `chesserazade_core`; a thin
  dispatcher over subcommands.
- `perft_bench` — a developer tool (built from `tools/`), also
  linking `chesserazade_core`. Not part of the test suite.

The test binary (`chesserazade_tests`) links the same library. The
library's public API (in `include/chesserazade/`) is what tests and
the CLI both consume — there is no "internal" shortcut around it for
production code. Tests are allowed to peek into `src/` to unit-test
the concrete `Board8x8Mailbox`, not just the abstract `Board`.

---

## 2. Layered architecture — today

```
+-----------------------------------------------------------+
|                       CLI / main                          |
|   src/main.cpp + src/cli/ — argument dispatch, one        |
|   cmd_*.cpp per subcommand: show, moves, perft, repl/play,|
|   solve, version                                          |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|        Search + TT + PuzzleSolver   (0.5–0.8)             |
|   Search::find_best — alpha-beta negamax with iterative   |
|   deepening, mate scoring, triangular PV, TT cutoffs,     |
|   move ordering (TT move + MVV-LVA + killers),            |
|   quiescence search on captures (0.8)                     |
|   TranspositionTable — Zobrist-keyed entry cache          |
|   PuzzleSolver::solve_mate_in — wraps Search for "mate    |
|   in N" puzzle semantics (0.8)                            |
|   evaluate(board) — material + piece-square tables        |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|              Game + PGN / SAN   (new in 0.4)              |
|   Game: Board + starting pos + vector<Move> history       |
|   SAN: parse/write Standard Algebraic Notation            |
|   PGN: parse/write Portable Game Notation                 |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|                     Move Generator                        |
|   generate_pseudo_legal, generate_legal,                  |
|   is_in_check, is_square_attacked                         |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|                     Board (interface)                     |
|   Board8x8Mailbox (0.1+)  |   BoardBitboard (planned 1.1) |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|                  Core types + FEN I/O                     |
|   Piece, Color, Square, File, Rank, Move, MoveKind,       |
|   CastlingRights — the vocabulary everybody uses          |
+-----------------------------------------------------------+
```

Layers that **do not yet exist** (planned per HANDOFF §9):

- **Game analyzer** (0.9), **net fetcher** (1.0).
- **Bitboard Board** (1.1), **Qt6 GUI** (1.2).

Each of these slots in *between* Move Generator and the CLI, without
requiring changes below it.

---

## 3. Data flow for the two commands that exist

### `chesserazade show --fen <fen>`

```
   FEN string ──► Board8x8Mailbox::from_fen ──► Board8x8Mailbox
                                                    │
                                                    ▼
                                           cli::cmd_show renders
                                           (ASCII or Unicode)
```

Only `from_fen` ever touches the concrete mailbox type in the CLI.
Rendering walks the abstract `Board` interface, so if 1.1's
`BoardBitboard` ships, nothing in `cmd_show` changes.

### `chesserazade perft --depth N --fen <fen>`

```
   FEN ──► Board8x8Mailbox
                │
                ▼
          recursive perft
                │
                ▼
   MoveGenerator::generate_legal ──► MoveList
                │
                ▼
   for m in list: board.make_move(m); perft(depth-1); board.unmake_move(m)
```

The recursion keeps a single mutable board and walks it via
`make_move` / `unmake_move` — **no copies per node**. The snapshot
history stack inside `Board8x8Mailbox` is what makes that safe.

---

## 4. Key abstractions

### `Board` (`include/chesserazade/board.hpp`)

Abstract class. Seven read-only queries (`piece_at`,
`side_to_move`, `castling_rights`, `en_passant_square`,
`halfmove_clock`, `fullmove_number` + `CastlingRights::any`) and two
mutators (`make_move`, `unmake_move`). The mutators push an
implementation-private snapshot so that `unmake_move` restores the
position exactly.

**Why abstract:** 1.1 will ship a bitboard implementation of the
same interface. Everything above this line — move generator, tests,
CLI — is written against `Board&`, so the swap is local.

### `Board8x8Mailbox` (`src/board/board8x8_mailbox.hpp`)

The concrete 0.x implementation: `array<Piece, 64>` indexed under
LERF (A1=0 … H8=63), plus scalar fields for side / castling / EP /
clocks. The `StateSnapshot` history vector holds the three fields
that a `Move` cannot reconstruct on its own (previous EP target,
castling rights, halfmove clock).

### `Move` + `MoveKind` (`include/chesserazade/move.hpp`)

A `Move` is `{ from, to, promotion, kind, moved_piece,
captured_piece }`. `MoveKind` is an 8-variant enum chosen to be
exhaustive and mutually exclusive: `Quiet`, `DoublePush`,
`KingsideCastle`, `QueensideCastle`, `Capture`, `EnPassant`,
`Promotion`, `PromotionCapture`. `make_move` switches on `kind` to
do the right side-effect; every other layer (UCI printing,
evaluation, search) can ignore `kind` if it wants.

### `MoveGenerator` (`include/chesserazade/move_generator.hpp`)

Stateless (`MoveGenerator() = delete`). Static methods:

- `generate_pseudo_legal(const Board&)` — every move the piece
  types allow, without regard for whether the moving side's king is
  left in check.
- `generate_legal(Board&)` — pseudo-legal, then filter by
  `make_move`/`is_in_check`/`unmake_move`. This is the only entry
  point most callers want.
- `is_in_check(const Board&, Color)` — scan for the king and ask
  whether its square is attacked.
- `is_square_attacked(const Board&, Square, attacker)` — the
  classical "look from the target" test: cast rays and jumps
  outward from `sq`; if the first piece along a ray or at a
  knight/pawn offset is the matching enemy piece type, return true.

### `MoveList` (same header)

Fixed-capacity `array<Move, 256>` plus a count. 256 is comfortably
above the theoretical maximum of 218 legal moves in any reachable
position. Rationale: a search alloctaes a `MoveList` per node;
stack allocation is cache-friendly and trivially recycled.

---

## 5. Directory layout (today)

```
chesserazade/
├── CMakeLists.txt              core library + chesserazade exe + perft_bench
├── CMakePresets.json           debug / release / release-with-asserts
├── HANDOFF.md                  implementation plan (+ docs/HANDOFF_pl.md)
├── README.md
├── LICENSE
│
├── include/chesserazade/       public headers — the library API
│   ├── board.hpp               abstract Board + CastlingRights
│   ├── fen.hpp                 serialize_fen + STARTING_POSITION_FEN
│   ├── move.hpp                Move, MoveKind, to_uci
│   ├── move_generator.hpp      MoveList + MoveGenerator
│   └── types.hpp               Color, Piece, Square, File, Rank
│
├── src/
│   ├── main.cpp
│   ├── cli/                    one cmd_*.cpp per subcommand
│   │   ├── command_dispatch.{hpp,cpp}
│   │   ├── cmd_show.{hpp,cpp}
│   │   ├── cmd_moves.{hpp,cpp}
│   │   ├── cmd_perft.{hpp,cpp}
│   │   └── cmd_version.{hpp,cpp}
│   ├── board/
│   │   └── board8x8_mailbox.{hpp,cpp}
│   ├── move_generator/
│   │   └── move_generator.cpp
│   └── io/
│       ├── fen.cpp
│       └── move.cpp
│
├── tests/                      Catch2 v3 via FetchContent
│   ├── CMakeLists.txt
│   ├── test_types.cpp
│   ├── test_move.cpp
│   ├── test_board.cpp
│   ├── test_fen.cpp
│   └── test_perft.cpp          six standard positions, depths 1..5
│
├── tools/
│   └── perft_bench.cpp         speed measurement, not run in tests
│
└── docs/
    ├── HANDOFF_pl.md
    ├── architecture.md         (this file) + architecture_pl.md
    ├── coding_style.md         + coding_style_pl.md
    └── version_notes/
        ├── 0.1.md + 0.1_pl.md
        ├── 0.2.md + 0.2_pl.md
        └── 0.3.md + 0.3_pl.md
```

Bilingual docs: the English file is canonical; `*_pl.md` is the
Polish translation kept in sync.

---

## 6. Design invariants

These are the contracts the code relies on. Breaking them is a bug;
tests assert several of them.

1. **LERF square mapping.** `Square` indices are rank-major,
   file-minor. `make_square(file, rank)`, `file_of`, `rank_of`, and
   the `A1=0 … H8=63` literals in `types.hpp` are the only
   conversions; no layer hand-rolls its own.
2. **FEN is ASCII.** Parsing rejects non-ASCII. Serialization emits
   only ASCII. Unicode figurines are a rendering choice, not a data
   format — the `--unicode` flag toggles them for display only.
3. **Move legality is the move generator's job.** `Board::make_move`
   assumes the caller has already filtered for legality. `make_move`
   does assert basic invariants (valid squares), but does not
   re-check that the move is legal in the chess-rules sense.
4. **`unmake_move` is exact.** After `make_move(m); unmake_move(m)`
   the board equals the pre-call board field-by-field (equality
   excludes the ephemeral history stack).
5. **Castling rights decay monotonically.** They can be cleared by
   king or rook moves, or by a capture onto a rook home corner.
   They are never re-granted within a game.
6. **No exceptions on hot paths.** Error signaling in the engine
   uses `std::expected` (FEN) or asserts (invariant violations).
   Exceptions are allowed only at the CLI boundary.
7. **Files stay under ~400 lines.** Growth past that is a signal to
   split by concept. (Not a hard rule, but a strong default.)

---

## 7. Where new subsystems plug in

When a later version lands, these are the extension points:

- **0.4 PGN / Game / repl** — `Game` sits *above* `Board`: it holds
  a `Board` plus a vector of `Move` for undo/redo and SAN display.
  `Pgn` is pure text I/O, independent of `Board`'s concrete type.
- **0.5 Evaluator** — new public header
  `include/chesserazade/evaluator.hpp`, concrete class in
  `src/eval/`. Takes `const Board&` only; no mutation.
- **0.5 Search** — new public header `search.hpp`, `src/search/`.
  Takes a mutable `Board&`, an `Evaluator&`, a limit, and a
  `MoveGenerator` implicitly (via static calls). Returns a
  `SearchResult`.
- **0.7 Zobrist + TT** — the hash key is maintained incrementally
  inside `Board`'s `make_move` / `unmake_move`. The TT is a new
  subsystem under `src/search/` that `Search` consults.
- **1.1 BoardBitboard** — a second implementation of the `Board`
  interface. The mailbox stays in the tree as the reference
  implementation; tests run against both via the abstract type.

The common thread: the `Board` interface is the stable contract;
every new subsystem either depends on that interface or sits above
it.

---

## 8. Things we deliberately did *not* do

- **No templates on `Board` / `MoveGenerator` / `Search`.** The
  polymorphism cost is negligible and a vtable is the more
  readable abstraction for this audience.
- **No "Position" / "State" / "Game" god-class in the core.** Each
  concern is its own type; `Game` (0.4) is a thin composition of
  `Board` + move history, not a rewrite.
- **No lambdas in hot code.** Named functions are easier to step
  through in a debugger and easier to grep for.
- **No external dependencies beyond Catch2.** The standard library
  covers everything we need in 0.x.
- **No NNUE, no neural networks.** Classical techniques only — that
  is the identity of the project.

# TODO

## ~~1.2 — UCI~~ (shipped, tag v1.2.0)

See `docs/version_notes/1.2.md`. Original plan kept below for
reference.

Reference: https://wbec-ridderkerk.nl/html/UCIProtocol.html

### What to implement

`chesserazade uci` — subcommand entering UCI mode. Line-based
stdin/stdout protocol.

**GUI → engine:**
- `uci` — handshake; reply with `id name chesserazade X.Y.Z`,
  `id author Andrzej Borucki`, enumerate options, `uciok`.
- `isready` — reply `readyok`.
- `ucinewgame` — reset TT and any search state.
- `setoption name <X> value <Y>` — at least:
  - `Hash` (TT size in MB) → drives `TranspositionTable` size
  - `Threads` (no-op — single-threaded)
- `position {startpos | fen <fen>} [moves m1 m2 ...]`
- `go` with:
  - `depth N`
  - `movetime ms`
  - `wtime X btime Y winc Z binc W movestogo N`
  - `infinite`
- `quit` — exit cleanly.

**Engine → GUI:**
- `info` after each completed ID iteration:
  `info depth N score cp X | mate N nodes N nps N time MS pv <uci>...`
- `bestmove <uci>` when the search ends.

### Scope decisions

- **Single-threaded.** Search blocks reading stdin. `stop` is
  not supported in this version — the engine respects its own
  time / node budget. Threading + `stop` = post-1.2.
- **Time management.** `go movetime ms` uses that budget
  directly. `go wtime/btime/winc/binc/movestogo` maps to
  a movetime via the classical `budget = time/30 + inc/2`.
  `go infinite` uses max depth.
- **Info output.** Per completed depth, not per node.
- **No multi-PV, no pondering, no Syzygy, no UCI_ShowWDL.**

### Commit breakdown (proposed)

1. **1.2.1 — UCI core**: reader/writer, handshake, `position`,
   `go depth/movetime`, `info`, `bestmove`. Ignore `stop` and
   complicated `go` variants.
2. **1.2.2 — Time management + setoption + docs + tag v1.2.0**:
   parse `wtime/btime/winc/binc/movestogo` → movetime.
   `setoption name Hash` steers TT size. Tests. Version notes
   1.2 (EN + PL). Tag v1.2.0.

### Deferred to a later version

- `stop` + threading (search on worker thread, reader on main).
- Multi-PV.
- Ponder.
- Syzygy tablebases.
- Polyglot opening book.
- Actual Qt6 GUI (the original 1.2 plan).

### Smoke-test path

Once implemented:
```
printf 'uci\nisready\nposition startpos moves e2e4\ngo depth 6\nquit\n' \
    | chesserazade uci
```
Then plug into Arena / CuteChess locally to confirm the UI side.

---

## Search polish — planned next etap (after 1.2 UCI)

Four pruning / ordering techniques to implement together as a
natural "search quality" version. Each is small (~100–300
lines + tests), classical, and well-documented. Expected
combined impact: roughly double the effective depth at the
same time budget.

### 1. History heuristic
Per-(piece, to-square) counter incremented on beta cutoffs
(scaled by `depth²`), used as the fourth move-ordering tier
after TT-move / MVV-LVA captures / killers. Closes the 0.8
move-ordering story (HANDOFF §9 0.8 originally listed it —
we shipped without it; killers + MVV-LVA were enough for
acceptance).
- Gain: 5–15% node reduction.
- Cost: tiny. An array, two update points, one comparator
  line in `score_move`.
- Reset on `ucinewgame` (or per-root-search).

### 2. PVS / NegaScout
First move at a node searched with full window `[α, β]`.
Subsequent moves searched with a **null window** `[α, α+1]`
first; if the null-window result > α, re-search with the
full window. Mathematically equivalent to alpha-beta given
good ordering, just faster.
- Gain: 5–15% node reduction.
- Cost: small diff inside negamax.
- Risk: essentially none.
- Reference: https://www.chessprogramming.org/Principal_Variation_Search

### 3. Null-move pruning
Before the usual search, try "passing" (flip side, no piece
moves) at reduced depth `depth - R - 1` (R = 2 or 3). If
that returns ≥ β the current position is already so good
that even giving the opponent a free move doesn't save them
— prune immediately.
- Gain: **biggest single win**, 20–40% effective depth at
  the same time.
- Cost: moderate — needs a guard band (no null-move when in
  check, in late-endgame material configurations, or when
  the previous move was null, to avoid zugzwang false
  positives).
- Needs a tiny `Board::make_null_move()` / `unmake_null_move()`
  that flips side + EP + zobrist without moving a piece.
- Reference: https://www.chessprogramming.org/Null_Move_Pruning

### 4. Checks in quiescence
Current quiescence explores captures only. Adding check-giving
moves catches short mating sequences the current qsearch
misses until the main search reaches them several plies
later.
- Gain: dramatic on tactical positions, none on quiet ones.
- Cost: moderate. Needs a check detector that doesn't make
  the move (`gives_check(board, move)`) or an after-move
  `is_in_check` probe, plus a guard to prevent tree
  explosion (e.g., a `check_budget` decrementing per ply, or
  "only checks at the first 1–2 plies of qsearch").
- Reference: https://www.chessprogramming.org/Quiescence_Search

### To consider (not committed)

- **LMR (Late Move Reductions).** Search late-in-the-list
  moves at reduced depth; if they return > α re-search at
  full depth. Gain 15–30%. Risk: may miss tactics on late
  moves. Many guard rules required (no reduction for TT
  move / captures / checks / moves-giving-check / near
  root). Tuning-heavy. Classical but *delicate*.
  Reference: https://www.chessprogramming.org/Late_Move_Reductions

### Not planned

- **Singular extensions.** Modest gain (5–10%),
  significant code, considerable tuning. Out of scope.
- **NNUE.** Out of scope per HANDOFF — undermines the
  classical / educational identity.

---

## Other

- Investigate whether `MoveGenerator`'s `dynamic_cast` dispatch
  in the hot path has any measurable cost on mailbox perft.
  If yes, replace with a virtual `Board::kind()` tag. (Note
  from 1.1 docs.)
- Optional: pinned-piece cache to skip the make+is_in_check+
  unmake legality filter. Big effort, large speedup. Not on
  the critical path.
- README section listing post-1.1 improvements actually
  shipped (magic/PEXT weren't in the original 1.1 scope).

# TODO

## ~~1.2 — UCI~~ (shipped, tag v1.2.0)

See `docs/version_notes/1.2.md`. Original plan kept below for
reference.

Reference: https://wbec-ridderkerk.nl/html/UCIProtocol.html

---

## ~~1.3 — Qt6 graphical analyzer~~ (shipped, tag v1.3.0)

See `docs/version_notes/1.3.md`. Original plan kept below for
reference.



Separate CMake target in the same repo (`chesserazade-analyzer`),
linking `chesserazade_core` directly as a library. **Not** a
play client — UCI is for that. This is for browsing games,
stepping through positions, and driving `Search` with a live
tree view.

Dependency: `find_package(Qt6 REQUIRED COMPONENTS Widgets
Concurrent)`. System-installed Qt6.

### Feature set

1. **Fetch dialog.**
   - Static player list from PGN Mentor shipped as
     `data/pgnmentor_index.json` (snapshot, refreshed manually).
     ~2000 entries.
     Tournaments + openings indices can follow the same pattern.
   - Cached items (file exists in `~/.cache/chesserazade/`)
     render with a distinct icon.
   - Pick player → download `<Name>.zip` → unzip → cache.

2. **Game list view.**
   - Lightweight PGN indexer (new `pgn_index.hpp` function): on
     a multi-game PGN, return `[{Event, White, Black, Date,
     Result, file_offset}]` without parsing moves. A 900-game
     PGN indexes in tens of ms.
   - Click a game → open the analyzer view.

3. **Analyzer view — board + move list + search tree.**
   - Board view (2D, from Qt widgets; drawing pieces via SVG or
     Unicode). Click a move in the list → position updates;
     back/forward buttons step through.
   - Move list widget synced with the board.
   - **Solve panel** — drives `Search::find_best` on the
     currently-visible position with user-picked budget:
     - max ply, or
     - time (sec), or
     - nodes (×1000 / ×1,000,000 multiplier).
     Search runs on a `QtConcurrent` worker; ID completed
     depths stream in as `info`-style lines (1, 2, 3, …) so the
     user sees the PV getting longer.

4. **Live search tree (the centerpiece).**
   - Custom `QAbstractItemModel` backed by a `TreeRecorder` fed
     by `Search`.
   - Only nodes at ply ≤ `lazy_ply_cap` (default 3–4) are
     materialised; deeper search runs recursively as normal
     and returns aggregated data per edge (score, capture
     sums, check counts on the best path).
   - Branches ordered by search order (reflects move
     ordering). Fail-high / cutoff node displayed with a
     distinct icon — "tu nastąpiło cięcie alfa-beta".
     Never-searched siblings simply don't appear (the branch
     is visibly narrower at a cut point).
   - Per-node fields (~17 B, ~25 B with container overhead):
     ```
     Move     from+to+promo+kind   ~3 B (packed)
     score    int32                4 B
     flags    bound | cutoff bit   1 B
     captures_white / black        2 × 4 B
     checks_white   / black        2 × 1 B
     ```
   - **Option A (initial):** record full tree to `lazy_ply_cap`
     in one pass during the search; GUI reads from memory on
     expand.
   - **Option B (polish):** when user expands beyond
     `lazy_ply_cap`, trigger a new sub-search from that
     position (TT shared). Keeps memory bounded at any search
     depth.

### Required additions to `chesserazade_core`

- **PGN**: new `pgn_index.hpp` / `pgn_index.cpp`:
  `[[nodiscard]] std::vector<PgnGameHeader> index_games(std::string_view pgn)`.
- **Search**: optional `TreeRecorder*` parameter on
  `Search::find_best`. When non-null, records nodes up to a
  `recorder->ply_cap()` and aggregates below. Recorder API:
  ```
  void enter_node(Move, int ply);
  void record_score(int score, TtBound bound);
  void record_capture(Color captor, int cp);
  void record_check(Color giver);
  void exit_node(bool was_cutoff);
  ```
- **Search**: capture-sum + check-counter tracking per branch
  (ProbeStats-style struct passed through recursion). Small
  overhead even when recorder is null — a couple of `if`
  checks per node. Acceptable.

### Deferred from 1.3

- **Threading + `stop` of an in-progress search** — user can
  wait or kill the GUI process in 1.3.0. Proper cancellation
  lands later (see §"Search polish / threading").
- **Editing positions by dragging pieces.** The analyzer reads
  positions from PGN; arbitrary FEN entry can be via a
  menu-level "paste FEN" in a later iteration.
- **Engine-vs-engine analysis comparison.**

### Sub-etap breakdown (proposed)

1. **1.3.1 — PGN indexer** (`pgn_index.{hpp,cpp}` + tests).
   Cheap indexing of multi-game PGN without parsing moves.
2. **1.3.2 — Search: TreeRecorder hook + capture/check counts**.
   Optional parameter on `Search::find_best`; tests verify
   recorded tree shape matches known small positions.
3. **1.3.3 — Qt6 skeleton**: CMake target, main window,
   About/Quit. Nothing functional; proves the build + link.
4. **1.3.4 — Fetch dialog**: static `data/pgnmentor_index.json`,
   player picker, download via existing `CurlFetcher`, cache
   indicator icons, unzip.
5. **1.3.5 — Game list view**: QTableView over the PGN indexer
   output, double-click opens analyzer view.
6. **1.3.6 — Board view + move list**: SVG/Unicode piece
   rendering, ←/→ nav, position-by-ply.
7. **1.3.7 — Solve panel (flat)**: budget picker, run on
   `QtConcurrent` worker, ID depth streams into a log view.
   No tree yet — smoke-level integration.
8. **1.3.8 — Live search tree**: QAbstractItemModel +
   QTreeView + TreeRecorder wiring, cutoff icon, lazy render
   at `lazy_ply_cap`. Tag v1.3.0.
9. **Post-1.3 (separate TODO):** lazy sub-search mode (Option
   B), true cancellation, position editing.

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

- **NNUE / neural eval.** Out of scope per HANDOFF — undermines
  the classical / educational identity.
- **Syzygy tablebases.** Out of scope for the 2.x line — data
  format + probe infrastructure is more plumbing than algorithm.

---

## 2.x — Stockfish-inspired classical additions

Background reading: `TECHNIKI_STOCKFISH.md` (paragraph numbers
in parentheses below). Everything here is classical — no NN,
no tablebases. Ordered roughly by expected bang-per-buck and
dependency chain, not by commit order.

### Foundations reused by everything else

- **Static Exchange Evaluation (SEE)** (2.7). Swap-list on the
  target square: cheap "will this capture sequence leave the
  captor ahead?" check. Blocks the door for five follow-on
  techniques: SEE-driven capture ordering (good/bad captures),
  SEE-filtered quiet ordering, futility filter in the parent,
  qsearch capture pruning, ProbCut candidate filter. Roughly
  100 lines + tests. Highest leverage per line of code here.

### Pruning heuristics

- **Mate Distance Pruning** (1.14). At every node, tighten
  α/β with `mated_in(ply)` / `mate_in(ply+1)`; if the window
  collapses return immediately. Three lines + one test.
  Nearly free, always correct.
- **Razoring** (1.6). Shallow depth, `eval < α − margin*depth²`
  → drop straight into qsearch. Simple, classical, modest
  gain. Pairs with the existing α-β mode combo so a user can
  see the pruning kick in.
- **Futility Pruning** (1.7). Two flavours:
  * *child-node* — before recursing, if `eval + margin < α`
    and the move can't capture or give check, prune.
  * *parent-node* — in the move loop, if the optimistic
    futility value stays ≤ α, prune the remaining quiet
    moves.
  Margin scales with depth and `improving`.
- **ProbCut** (1.12). On a reduced depth, a capture that
  already produces `score ≥ β + margin` is taken as a hard
  cut on the entire branch. SEE-filtered candidate list;
  qsearch verification before the full search. Classical
  (Buro, 1995) and fits 2.x cleanly.
- **Multi-Cut** (1.10). Free rider on Singular Extensions
  (same singular-beta test): if multiple moves return
  `≥ singular_beta`, the position is multi-cut — return
  that value immediately. ~30 extra lines once SE is in.

### Depth adjustment

- **Internal Iterative Reductions (IIR)** (1.13). In PV /
  cut nodes with no TT-move at sufficient depth, search
  with `depth − 1`. Forces the next iteration to populate
  the TT, which then drives better ordering on re-visit.
  A modern, shorter replacement for classical IID.

### Move-ordering refinements

Additional history tables, each indexed differently, each
feeding into the move picker's sort key:

- **Continuation History** (2.5). Tables of "previous move →
  current move" pairs at multiple stack offsets (typically
  1 and 2 ply back). Captures "after they play X, Y is
  usually strong for me" patterns that butterfly history
  can't see.
- **Capture History** (2.4). `[piece][to][captured_type]`.
  Stockfish uses it alongside MVV to rank captures — a
  Bxc3 that keeps hanging doesn't float to the top just
  because it takes a piece.
- **Pawn History** (2.6). Indexed by `pawn_key`: strategic
  context across positions with the same pawn skeleton.
  Useful mostly in the middlegame.
- **Low-Ply History** (2.3). Separate, faster-decaying
  table for ply ≤ N; reacts quickly to the root move
  changing between iterations.

### Meta / integration

- **`improving` / `opponentWorsening` flags** (8.4). Compare
  current static eval to `(ss − 2)->staticEval` and
  `−(ss − 1)->staticEval`. Fed into margins for futility /
  NMP / LMR — lots of heuristics get a "widen the margin when
  the trend is against us" knob for free once these flags
  exist.
- **Correction History** (4.3). Track the observed gap
  between `staticEval` and the eventual search score,
  indexed by pawn-key / minor-piece-key / continuation.
  Add that correction back into `staticEval` on the next
  visit. Elegant bridge between static eval and search;
  cheap to compute.
- **Aspiration Windows (Stockfish-style)** (1.4). We had a
  simple version and removed it in v1.4 for being net-zero.
  The full version is: track `iterValue[depth]`, compute
  `delta` growing exponentially on fail-high / fail-low,
  centre the window on the previous iteration's score.
  Only worth re-adding after enough pruning is in place
  that the wide-window search visibly stalls.

### Observability

- **seldepth** (8.3). Track the deepest ply any branch
  actually visited inside one ID iteration (after
  extensions, before qsearch leaves). Expose it in the
  solve-panel log line alongside `depth` so the user can
  see how much the extensions are digging past the nominal.
- **cutoffCnt** (8.5). Per-node running count of β-cutoffs
  in its subtree. Useful on its own for the analyzer's
  tree view, and a direct input into LMR once that lands
  (a node that's been cutting a lot gets less reduction).

### Low priority — kept on the list for completeness

- **Singular Extensions** (1.9). TT-move + singular-beta test
  at reduced window/depth; if no other move beats singular-
  beta, extend the TT-move by +1 ply (or more for wide
  margins). Stockfish measures it in the 15–20 Elo range;
  in a simpler engine the absolute gain is smaller but the
  algorithm is still classical and well-documented. Pairs
  naturally with LMR — SE extends critical TT-moves just as
  LMR shortens the others. Implementation needs an
  `excluded_move` parameter on negamax and ~1 tuned margin
  parameter; ~200 lines + tests. Deferred until LMR lands,
  because without LMR there's not much to compensate for.
  Not a first target, but no longer in "not planned".

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

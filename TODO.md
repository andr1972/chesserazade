# TODO

## 1.2 — UCI (Universal Chess Interface)

Replaces the original 1.2 plan (Qt6 GUI). The Qt GUI is deferred
indefinitely; instead we make `chesserazade` pluggable into
existing GUIs (Arena, CuteChess, SCID) via the standard UCI
protocol.

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

## Other

- Investigate whether `MoveGenerator`'s `dynamic_cast` dispatch
  in the hot path has any measurable cost on mailbox perft.
  If yes, replace with a virtual `Board::kind()` tag. (Note
  from 1.1 docs.)
- Optional: pinned-piece cache to skip the make+is_in_check+
  unmake legality filter. Big effort, large speedup. Not on
  the critical path.
- Optional: quiescence including checks (currently captures-
  only). Improves tactical depth, explodes tree size.
- Optional: history heuristic (third move-ordering tier).
- README section listing post-1.1 improvements actually
  shipped (magic/PEXT weren't in the original 1.1 scope).

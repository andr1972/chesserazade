// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Alpha-beta negamax with iterative deepening, TT cutoffs,
/// move ordering, and quiescence search.
///
/// Algorithmic layers, in the order they were added:
///   0.5 — plain negamax.
///   0.6 — alpha-beta pruning, iterative deepening, time/node
///         limits.
///   0.7 — Zobrist-keyed transposition table (probe + store).
///   0.8 — move ordering (TT move first, MVV-LVA for captures,
///         two killer-move slots per ply), quiescence search on
///         captures at the horizon.
///
/// Why move ordering matters: alpha-beta is maximally effective
/// when the best move is searched first — in the theoretical
/// optimum the tree shrinks from `b^d` to `b^(d/2)`, a square-
/// root speedup. Without ordering we probe the same positions in
/// "whichever order the generator happens to emit"; with it the
/// TT-suggested move and high-MVV captures get searched first and
/// prune the rest.
///
/// Why quiescence: the horizon effect. Without it, a search that
/// ends in a position where a capture sequence is in flight will
/// score the pre-capture material as final ("I'll evaluate before
/// the queen gets traded off"). Quiescence keeps searching
/// *tactical* (capture) moves past the nominal depth until the
/// position is calm, so the evaluator only runs on truly quiet
/// leaves.
///
/// References:
///   https://www.chessprogramming.org/Move_Ordering
///   https://www.chessprogramming.org/MVV-LVA
///   https://www.chessprogramming.org/Killer_Heuristic
///   https://www.chessprogramming.org/Quiescence_Search
#include <chesserazade/search.hpp>

#include <chesserazade/board.hpp>
#include <chesserazade/evaluator.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/move_generator.hpp>
#include <chesserazade/transposition_table.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace chesserazade {

namespace {

using clk = std::chrono::steady_clock;

constexpr std::size_t PV_SIZE = static_cast<std::size_t>(Search::MAX_DEPTH);

struct PvTable {
    std::array<std::array<Move, PV_SIZE>, PV_SIZE> moves{};
    std::array<std::size_t, PV_SIZE> length{};
};

/// Return value of `negamax` / `quiesce`. `score` has the usual
/// fail-soft semantics; `exact` is true iff the move loop at this
/// node visited all direct children (no β-cutoff break). Children
/// may themselves be bounds — `exact` only describes the loop at
/// *this* node, not the whole subtree. At a TT probe-cut `exact`
/// is reconstructed from the stored bound: `Lower` (fail-high
/// when stored) → false, anything else → true.
struct NegamaxResult {
    int score = 0;
    bool exact = false;
};

/// Two killer-move slots per ply. A "killer" is a quiet move that
/// caused a beta cutoff at the same ply of a sibling node — the
/// heuristic bet is that it will cut again here. We keep only the
/// two most recent and never duplicate the slot-0 killer into
/// slot-1.
struct KillerTable {
    std::array<std::array<Move, 2>, PV_SIZE> killers{};
};

/// History heuristic — `[color][from][to]` score, bumped by
/// `depth^2` when a quiet move triggers a β-cutoff. The same
/// `(from, to)` pair reappears in many positions across a
/// single search tree, so the table converges on a useful
/// "which quiet moves have been cutting lately" ordering
/// signal after a few thousand nodes. Unlike killers (per-ply
/// slots), history is global across plies and survives for
/// the whole `find_best` call. Cleared at search start by
/// zero-init; no persistence between searches — the table
/// means nothing for an unrelated root position.
/// See https://www.chessprogramming.org/History_Heuristic
struct HistoryTable {
    std::array<std::array<std::array<std::int32_t, 64>, 64>, 2> table{};

    [[nodiscard]] std::int32_t get(Color stm,
                                   Square from, Square to) const noexcept {
        return table[static_cast<std::size_t>(stm)]
                    [to_index(from)][to_index(to)];
    }
    void bump(Color stm, Square from, Square to, int depth) noexcept {
        table[static_cast<std::size_t>(stm)]
             [to_index(from)][to_index(to)] += depth * depth;
    }
};

/// Budget enforcement. A single `abort` flag short-circuits the
/// recursion; we check the clock / node count only on a power-of-
/// two boundary to keep the per-node overhead negligible.
struct Stop {
    clk::time_point start;
    std::chrono::milliseconds time_budget;
    std::uint64_t node_budget;
    bool disable_alpha_beta = false;
    bool disable_quiescence = false;
    bool root_full_window   = false;
    bool use_incremental_eval = false;
    bool enable_lmr = false;
    std::atomic<bool>* cancel = nullptr;
    std::atomic<std::uint64_t>* progress_nodes = nullptr;
    bool abort = false;

    bool should_stop(std::uint64_t nodes_so_far) noexcept {
        if (abort) return true;
        if (progress_nodes != nullptr) {
            progress_nodes->store(nodes_so_far,
                                  std::memory_order_relaxed);
        }
        if (cancel != nullptr
            && cancel->load(std::memory_order_relaxed)) {
            abort = true;
            return true;
        }
        if (node_budget > 0 && nodes_so_far >= node_budget) {
            abort = true;
            return true;
        }
        if (time_budget.count() > 0) {
            if (clk::now() - start >= time_budget) {
                abort = true;
                return true;
            }
        }
        return false;
    }
};

constexpr std::uint64_t STOP_CHECK_MASK = 2047;

// ---------------------------------------------------------------------------
// Mate-score translation (unchanged from 0.7)
// ---------------------------------------------------------------------------

[[nodiscard]] int to_tt_score(int score, int ply) noexcept {
    if (score >= Search::MATE_SCORE - 1000) return score + ply;
    if (score <= -(Search::MATE_SCORE - 1000)) return score - ply;
    return score;
}
[[nodiscard]] int from_tt_score(int score, int ply) noexcept {
    if (score >= Search::MATE_SCORE - 1000) return score - ply;
    if (score <= -(Search::MATE_SCORE - 1000)) return score + ply;
    return score;
}

// ---------------------------------------------------------------------------
// Move classification + ordering
// ---------------------------------------------------------------------------

[[nodiscard]] bool is_capture(const Move& m) noexcept {
    return m.kind == MoveKind::Capture
        || m.kind == MoveKind::EnPassant
        || m.kind == MoveKind::PromotionCapture;
}

/// Centipawn value of the piece removed by `m`, or 0 for non-
/// captures. En-passant captures always take a pawn.
[[nodiscard]] int capture_value(const Move& m) noexcept {
    switch (m.kind) {
        case MoveKind::Capture:
        case MoveKind::PromotionCapture:
            return piece_value(m.captured_piece.type);
        case MoveKind::EnPassant:
            return piece_value(PieceType::Pawn);
        default:
            return 0;
    }
}

/// MVV-LVA score: 10 * value(victim) - value(attacker). Rewards
/// trading our cheap piece for their expensive one. En-passant
/// captures always take a pawn so we plug in PAWN explicitly.
[[nodiscard]] int mvv_lva(const Move& m) noexcept {
    const PieceType victim =
        (m.kind == MoveKind::EnPassant) ? PieceType::Pawn
                                        : m.captured_piece.type;
    return 10 * piece_value(victim) - piece_value(m.moved_piece.type);
}

/// Ordering priority buckets. Higher is earlier.
///   ~10'000'000 — TT move (the previous best move at this key).
///   ~ 1'000'000 — captures (spread by MVV-LVA).
///   ~   900'000 — first killer.
///   ~   800'000 — second killer.
///   ~    50'000 — promotions without capture.
///   0..49'999   — quiet moves, ranked by history heuristic.
[[nodiscard]] int score_move(const Move& m, const Move& tt_move,
                             const KillerTable& killers,
                             const HistoryTable& history,
                             Color stm,
                             std::size_t ply) noexcept {
    if (m == tt_move && tt_move.from != Square::None) {
        return 10'000'000;
    }
    if (is_capture(m)) {
        return 1'000'000 + mvv_lva(m);
    }
    if (m == killers.killers[ply][0]) return 900'000;
    if (m == killers.killers[ply][1]) return 800'000;
    if (m.kind == MoveKind::Promotion) {
        return 50'000 + piece_value(m.promotion);
    }
    // Quiet move: use history. Clamped below the promotion
    // bucket so a very old, over-accumulated (from,to) can't
    // leapfrog categories. LMR keys on "score == 0" to detect
    // genuinely untested quiets, so we also floor at 0.
    const std::int32_t h = history.get(stm, m.from, m.to);
    if (h <= 0) return 0;
    return h < 49'999 ? static_cast<int>(h) : 49'999;
}

struct ScoredMove {
    Move move;
    int score;
};

[[nodiscard]] bool scored_desc(const ScoredMove& a,
                               const ScoredMove& b) noexcept {
    return a.score > b.score;
}

/// Fill `buf` with `(move, score)` pairs and sort it in
/// descending order. Returns the number of entries written.
std::size_t score_and_sort(const MoveList& legal,
                           std::array<ScoredMove, MoveList::CAPACITY>& buf,
                           const Move& tt_move,
                           const KillerTable& killers,
                           const HistoryTable& history,
                           Color stm,
                           std::size_t ply) noexcept {
    const std::size_t n = legal.count;
    for (std::size_t i = 0; i < n; ++i) {
        const Move& m = legal.moves[i];
        buf[i] = {m, score_move(m, tt_move, killers, history, stm, ply)};
    }
    std::sort(buf.begin(), buf.begin() + n, scored_desc);
    return n;
}

/// Record a killer at `ply`. Drop the previous slot-0 into
/// slot-1 (no duplicate). Skip captures — they already sort high
/// via MVV-LVA and a killer slot is scarce.
void remember_killer(KillerTable& killers, std::size_t ply,
                     const Move& m) noexcept {
    if (is_capture(m)) return;
    if (killers.killers[ply][0] == m) return;
    killers.killers[ply][1] = killers.killers[ply][0];
    killers.killers[ply][0] = m;
}

/// Bump history for a quiet move that caused a β-cutoff. Only
/// quiet moves qualify — captures / promotions already sort
/// high by their own buckets and polluting history with them
/// would wash out the signal for genuinely quiet winners.
void remember_history(HistoryTable& history, Color stm,
                      const Move& m, int depth) noexcept {
    if (is_capture(m)) return;
    if (m.kind == MoveKind::Promotion) return;
    history.bump(stm, m.from, m.to, depth);
}

/// Add two `BranchStats` field-wise. Small enough that the
/// compiler inlines the four adds.
[[nodiscard]] BranchStats combine(const BranchStats& a,
                                  const BranchStats& b) noexcept {
    return {
        a.captures_white + b.captures_white,
        a.captures_black + b.captures_black,
        a.checks_white   + b.checks_white,
        a.checks_black   + b.checks_black,
    };
}

// ---------------------------------------------------------------------------
// Quiescence search
// ---------------------------------------------------------------------------
//
// At the horizon of the main search we do not trust the static
// evaluator in the middle of a capture sequence. Instead we
// continue searching *tactical* replies only (captures, for 0.8)
// until the position stabilises.
//
// The classical "stand-pat" technique: take the static eval as a
// lower bound on the value of this node. If it already exceeds
// beta we can cut; otherwise use it as the initial alpha while we
// look for captures that improve on it.

NegamaxResult quiesce(Board& board, int alpha, int beta,
                      std::uint64_t& nodes, Stop& stop,
                      BranchStats& out_stats);

NegamaxResult quiesce(Board& board, int alpha, int beta,
                      std::uint64_t& nodes, Stop& stop,
                      BranchStats& out_stats) {
    ++nodes;
    out_stats = {};
    if ((nodes & STOP_CHECK_MASK) == 0 && stop.should_stop(nodes)) {
        return {0, false};
    }

    const int stand_pat = stop.use_incremental_eval
        ? board.evaluate_incremental()
        : evaluate(board);
    // Fail-soft: return the actual value we know, not just the
    // window bound. Keeps the correctness of α-β (the caller
    // only needs "≥ beta" for a cut) while letting the tree
    // view show the true magnitude of bad moves.
    //
    // Stand-pat cut counts as non-exact: we short-circuit
    // before even looking at any capture reply, so from the
    // "visited all direct children" point of view zero of N
    // were visited.
    // Quiescence always runs with α-β, independently of
    // `disable_alpha_beta`. Without cutoffs, a capture / re-
    // capture sequence with positive SEE would expand the
    // tree without bound, so plain minimax here would not
    // terminate in any useful time.
    if (stand_pat >= beta) {
        return {stand_pat, false};
    }
    if (stand_pat > alpha) alpha = stand_pat;

    const MoveList legal = MoveGenerator::generate_legal(board);

    // Score only captures; skip quiet moves entirely.
    std::array<ScoredMove, MoveList::CAPACITY> buf;
    std::size_t n = 0;
    for (std::size_t i = 0; i < legal.count; ++i) {
        const Move& m = legal.moves[i];
        if (!is_capture(m)) continue;
        buf[n++] = {m, mvv_lva(m)};
    }
    std::sort(buf.begin(), buf.begin() + n, scored_desc);

    BranchStats best_stats;
    int best_score = stand_pat; // stand-pat is the floor

    // Counts captures whose recursive call fully returned
    // (incremented *before* the possible β-cut break so that
    // a cut on the final capture still yields `visited == n`
    // and therefore `exact == true`).
    std::size_t visited = 0;

    for (std::size_t i = 0; i < n; ++i) {
        const Move& m = buf[i].move;
        const Color mover = board.side_to_move();

        BranchStats delta;
        const int cap_val = capture_value(m);
        if (cap_val > 0) {
            if (mover == Color::White) delta.captures_white = cap_val;
            else                       delta.captures_black = cap_val;
        }

        board.make_move(m);
        BranchStats child_stats;
        const NegamaxResult child =
            quiesce(board, -beta, -alpha, nodes, stop, child_stats);
        const int score = -child.score;
        board.unmake_move(m);
        if (stop.abort) return {0, false};

        ++visited;

        const BranchStats combined = combine(delta, child_stats);
        if (score > best_score) {
            best_score = score;
            best_stats = combined;
        }
        if (score > alpha) alpha = score;
        if (score >= beta) { // see note above — quiescence α-β is forced on.
            out_stats = combined;
            return {score, visited == n};
        }
    }
    out_stats = best_stats;
    return {best_score, true};
}

// ---------------------------------------------------------------------------
// Null-move pruning helpers
// ---------------------------------------------------------------------------

/// True when `side` has at least one piece that is not a king or
/// pawn. In pure king+pawn endgames NMP is unsound — the "pass"
/// can be strictly worse than any real move (zugzwang). A quick
/// full-board scan through `piece_at` is fine here: NMP runs at
/// most once per interior node and the scan is 64 reads.
[[nodiscard]] bool has_non_pawn_material(const Board& b, Color side) {
    for (std::size_t i = 0; i < NUM_SQUARES; ++i) {
        const Piece p = b.piece_at(static_cast<Square>(i));
        if (p.type == PieceType::None) continue;
        if (p.color != side) continue;
        if (p.type != PieceType::Pawn && p.type != PieceType::King) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Main negamax
// ---------------------------------------------------------------------------

NegamaxResult negamax(Board& board, int depth, int ply, int alpha, int beta,
                      std::uint64_t& nodes, PvTable& pv, KillerTable& killers,
                      HistoryTable& history,
                      Stop& stop, TranspositionTable* tt,
                      TreeRecorder* rec, BranchStats& out_stats,
                      bool allow_null = true) {
    ++nodes;
    out_stats = {};

    const std::size_t p = static_cast<std::size_t>(ply);
    pv.length[p] = 0;

    if ((nodes & STOP_CHECK_MASK) == 0 && stop.should_stop(nodes)) {
        return {0, false};
    }

    // --- TT probe -----------------------------------------------------
    // On a probe-cut we reconstruct `exact` from the stored bound.
    // Loop-completed stores use `Exact` (best > α₀) or `Upper`
    // (fail-low, all children visited) — both mean "all direct
    // children were visited" in our sense. A `Lower` store only
    // happens at a β-cutoff, which is precisely `exact=false`.
    const ZobristKey key = board.zobrist_key();
    Move tt_move{};
    if (tt != nullptr) {
        const TtProbe probe = tt->probe(key);
        if (probe.hit) {
            tt_move = probe.entry.move;
            if (ply > 0
                && static_cast<int>(probe.entry.depth) >= depth) {
                const int s = from_tt_score(probe.entry.score, ply);
                const bool tt_exact =
                    (probe.entry.bound() != TtBound::Lower);
                switch (probe.entry.bound()) {
                    case TtBound::Exact: return {s, tt_exact};
                    case TtBound::Lower:
                        if (!stop.disable_alpha_beta && s >= beta)
                            return {s, tt_exact};
                        break;
                    case TtBound::Upper:
                        if (!stop.disable_alpha_beta && s <= alpha)
                            return {s, tt_exact};
                        break;
                    case TtBound::None:  break;
                }
            }
        }
    }

    // --- Horizon: quiesce ---------------------------------------------
    // `depth <= 0` rather than `== 0` because pruning reductions
    // (NMP with R=3, LMR later) can push the caller's depth
    // into the negatives. Treat every such case as horizon so
    // the recursion terminates regardless of reducer size.
    if (depth <= 0) {
        if (stop.disable_quiescence) {
            // Raw static eval — horizon effect visible. User
            // opt-in to see what "pure depth-N minimax" gives.
            const int e = stop.use_incremental_eval
                ? board.evaluate_incremental()
                : evaluate(board);
            return {e, true}; // a static-eval leaf has no children to miss.
        }
        // Pass out_stats through so quiescence captures /
        // recaptures are reflected in the parent's tree view
        // — otherwise the Captures columns under-report (a
        // rook-takes-queen with quiesce-recapture would show
        // W:900, B:0 while the score is 0).
        return quiesce(board, alpha, beta, nodes, stop, out_stats);
    }

    // --- Null-move pruning --------------------------------------------
    // "Pass" to the opponent with depth - R - 1 and a null window
    // around beta. If the opponent can't beat beta even when given
    // the move for free, the true value of this position is
    // already ≥ beta; prune the whole subtree.
    //
    // Guards:
    //   * `allow_null` — never two nulls in a row (zugzwang cascade).
    //   * α-β must be on — NMP is a pruning technique, no pruning
    //     mode means no NMP.
    //   * Not at the root — we need real moves there.
    //   * Depth ≥ 3 — below that the depth budget after R+1 is
    //     thin and the savings are small.
    //   * Not in check — "passing" while in check is illegal.
    //   * Side to move has non-pawn material — in king+pawns
    //     endgames any "move" can be strictly worse than passing
    //     (zugzwang), so NMP is unsound there.
    //   * Static eval ≥ beta — if we're already below beta, a
    //     free move for the opponent certainly won't push us
    //     above it; no point paying for the reduced-depth search.
    //
    // Reducer R = 3 (classical; deeper formulas belong in 2.x
    // polish). The pruning branch is a shadow search — we pass
    // `rec = nullptr` so it does not pollute the analyzer tree.
    constexpr int NMP_R = 3;
    if (allow_null
        && !stop.disable_alpha_beta
        && ply > 0
        && depth >= 3
        && !MoveGenerator::is_in_check(board, board.side_to_move())
        && has_non_pawn_material(board, board.side_to_move())) {
        const int static_eval = stop.use_incremental_eval
            ? board.evaluate_incremental()
            : evaluate(board);
        if (static_eval >= beta) {
            board.make_null_move();
            BranchStats null_stats;
            const NegamaxResult nr = negamax(
                board, depth - NMP_R - 1, ply + 1,
                -beta, -beta + 1,
                nodes, pv, killers, history, stop, tt,
                /*rec=*/nullptr, null_stats,
                /*allow_null=*/false);
            board.unmake_null_move();

            if (stop.abort) return {0, false};

            const int null_score = -nr.score;
            if (null_score >= beta) {
                // Fail-soft prune. `exact=false` because we
                // didn't visit a single real child — the whole
                // node is a bound, not a value.
                return {null_score, false};
            }
        }
    }

    // --- Legality + terminal ------------------------------------------
    const MoveList legal = MoveGenerator::generate_legal(board);
    if (legal.empty()) {
        if (MoveGenerator::is_in_check(board, board.side_to_move())) {
            return {-Search::MATE_SCORE + ply, true};
        }
        return {0, true}; // stalemate
    }

    // --- Move ordering ------------------------------------------------
    std::array<ScoredMove, MoveList::CAPACITY> buf;
    const std::size_t n =
        score_and_sort(legal, buf, tt_move, killers, history,
                       board.side_to_move(), p);

    const int original_alpha = alpha;
    Move best_move{};
    BranchStats best_stats;
    // `best_score` tracks the best result actually observed in
    // this subtree, independent of the starting α. Returning
    // it (fail-soft) preserves the true magnitude for fail-
    // low nodes: a move that's clearly worse than the current
    // best at root no longer masquerades as the alpha bound.
    int best_score = -Search::INF_SCORE;

    // Counts children whose recursive call fully returned
    // (incremented *before* the possible β-cut break so that
    // a cut on the final child still yields `visited == n`
    // and therefore `exact == true`).
    std::size_t visited = 0;

    const int child_ply = ply + 1;
    const bool report_children =
        (rec != nullptr) && child_ply <= rec->ply_cap();

    for (std::size_t i = 0; i < n; ++i) {
        const Move& m = buf[i].move;
        const Color mover = board.side_to_move();

        // Per-move stat delta. Capture value is free from the
        // move itself; check detection needs an after-move probe
        // and is only paid when a recorder is attached.
        BranchStats delta;
        const int cap_val = capture_value(m);
        if (cap_val > 0) {
            if (mover == Color::White) delta.captures_white = cap_val;
            else                       delta.captures_black = cap_val;
        }

        if (report_children) rec->enter(child_ply, m);

        board.make_move(m);

        bool gives_check = false;
        if (rec != nullptr) {
            // Did this move give check? Probe the post-move
            // board: the side *to* move is now the opponent;
            // we ask whether they are in check.
            const Color them = board.side_to_move();
            if (MoveGenerator::is_in_check(board, them)) {
                gives_check = true;
                if (mover == Color::White) delta.checks_white = 1;
                else                       delta.checks_black = 1;
            }
        }

        // Snapshot the α the child will actually see. `alpha`
        // may have improved earlier in this loop; that current
        // value is what drives the recursive call's window, and
        // it is what the child observed at its root. Persisting
        // it lets a lazy sub-search seed the identical window.
        //
        // When `root_full_window` is set AND we're at ply 0,
        // give each child a fresh full window so every root
        // move gets an exact score, not a bound influenced by
        // earlier siblings.
        int recurse_alpha = -beta;
        int recurse_beta  = -alpha;
        if (stop.root_full_window && ply == 0) {
            recurse_alpha = -Search::INF_SCORE;
            recurse_beta  =  Search::INF_SCORE;
        }
        const int child_alpha = recurse_alpha;
        const int child_beta  = recurse_beta;

        // Snapshot the global node counter so we can report the
        // child subtree's visit count — counts alpha-beta-cut
        // nodes and quiescence + sub-cap nodes too.
        const std::uint64_t nodes_before = nodes;

        // --- LMR: probe late quiet moves at reduced depth ---
        // A "late" move here means one ordered at index ≥ 3
        // (after TT / captures / killers) AND scored as a plain
        // quiet (buf[i].score == 0) — so captures, killers and
        // promotions are automatically excluded by the bucket,
        // not by re-testing move kind. The probe runs with the
        // recorder detached, like null-move pruning: if it
        // fails low (score ≤ alpha) we accept it as truth and
        // the tree view shows this branch as a leaf; if it
        // beats alpha we re-search at full depth with the
        // recorder attached, so the tree reflects the
        // confirmed subtree exactly once.
        const bool lmr_candidate =
               stop.enable_lmr
            && !stop.disable_alpha_beta
            && depth >= 3
            && i >= 3
            && buf[i].score == 0;
        const int R = lmr_candidate ? 1 : 0;
        BranchStats child_stats;
        NegamaxResult child =
            negamax(board, depth - 1 - R, child_ply,
                    recurse_alpha, recurse_beta,
                    nodes, pv, killers, history, stop, tt,
                    (R > 0 ? nullptr : rec), child_stats);
        int score = -child.score;
        if (R > 0 && !stop.abort && score > alpha) {
            child_stats = {};
            child = negamax(board, depth - 1, child_ply,
                            recurse_alpha, recurse_beta,
                            nodes, pv, killers, history, stop, tt,
                            rec, child_stats);
            score = -child.score;
        }
        board.unmake_move(m);

        const BranchStats combined = combine(delta, child_stats);

        if (report_children) {
            const bool caused_cutoff = (score >= beta);
            rec->leave(child_ply, score, caused_cutoff, combined,
                       /*remaining_depth=*/depth - 1,
                       child_alpha, child_beta,
                       /*subtree_nodes=*/nodes - nodes_before,
                       gives_check, child.exact);
        }

        if (stop.abort) return {0, false};

        ++visited;

        if (score > best_score) {
            best_score = score;
            best_move = m;
            best_stats = combined;
            // Refresh PV on every genuine improvement — not
            // only when alpha moves, so fail-low-but-best-
            // among-bad still carries a PV through.
            pv.moves[p][0] = m;
            const std::size_t child_len = pv.length[p + 1];
            for (std::size_t j = 0; j < child_len; ++j) {
                pv.moves[p][j + 1] = pv.moves[p + 1][j];
            }
            pv.length[p] = 1 + child_len;
        }
        if (score > alpha) alpha = score;

        if (!stop.disable_alpha_beta && score >= beta) {
            remember_killer(killers, p, m);
            remember_history(history, mover, m, depth);
            if (tt != nullptr) {
                tt->store(key, depth, to_tt_score(score, ply),
                          TtBound::Lower, m);
            }
            out_stats = combined;
            // Cutoff on the last child still visited every
            // direct child — mark exact in that edge case.
            return {score, visited == n};
        }
    }

    if (tt != nullptr) {
        const TtBound bound = (best_score > original_alpha)
                                  ? TtBound::Exact
                                  : TtBound::Upper;
        tt->store(key, depth, to_tt_score(best_score, ply),
                  bound, best_move);
    }

    out_stats = best_stats;
    return {best_score, true}; // loop completed → every direct child visited.
}

int iteration(Board& board, int depth, std::uint64_t& nodes,
              PvTable& pv, KillerTable& killers,
              HistoryTable& history,
              Stop& stop, TranspositionTable* tt,
              TreeRecorder* rec, BranchStats& out_stats,
              int alpha, int beta) {
    const NegamaxResult r =
        negamax(board, depth, /*ply=*/0,
                alpha, beta,
                nodes, pv, killers, history, stop, tt,
                rec, out_stats);
    return r.score;
}

} // namespace

SearchResult Search::find_best(Board& board, int depth) {
    SearchLimits l;
    l.max_depth = depth;
    return find_best(board, l, nullptr, nullptr,
                     -INF_SCORE, INF_SCORE);
}

SearchResult Search::find_best(Board& board, const SearchLimits& limits) {
    return find_best(board, limits, nullptr, nullptr,
                     -INF_SCORE, INF_SCORE);
}

SearchResult Search::find_best(Board& board, const SearchLimits& limits,
                               TranspositionTable* tt) {
    return find_best(board, limits, tt, nullptr,
                     -INF_SCORE, INF_SCORE);
}

SearchResult Search::find_best(Board& board, const SearchLimits& limits,
                               TranspositionTable* tt,
                               TreeRecorder* recorder) {
    return find_best(board, limits, tt, recorder,
                     -INF_SCORE, INF_SCORE);
}

SearchResult Search::find_best(Board& board, const SearchLimits& limits,
                               TranspositionTable* tt,
                               TreeRecorder* recorder,
                               int alpha, int beta) {
    SearchResult result;

    int max_depth = limits.max_depth;
    if (max_depth < 0) max_depth = 0;
    if (max_depth > MAX_DEPTH) max_depth = MAX_DEPTH;

    Stop stop{
        clk::now(),
        limits.time_budget,
        limits.node_budget,
        limits.disable_alpha_beta,
        limits.disable_quiescence,
        limits.root_full_window,
        limits.use_incremental_eval,
        limits.enable_lmr,
        limits.cancel,
        limits.progress_nodes,
        false,
    };

    // Root terminal.
    const MoveList root_moves = MoveGenerator::generate_legal(board);
    if (root_moves.empty()) {
        if (MoveGenerator::is_in_check(board, board.side_to_move())) {
            result.score = -MATE_SCORE;
        } else {
            result.score = 0;
        }
        result.nodes = 1;
        result.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                clk::now() - stop.start);
        return result;
    }

    if (max_depth == 0) {
        result.best_move = *root_moves.begin();
        result.score = evaluate(board);
        result.nodes = 1;
        result.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                clk::now() - stop.start);
        return result;
    }

    if (tt != nullptr) tt->new_search();

    const std::uint64_t probes_before = tt != nullptr ? tt->probes() : 0;
    const std::uint64_t hits_before   = tt != nullptr ? tt->hits()   : 0;

    // Killers are kept across ID iterations of the same search —
    // a move that cut the sibling at ply N in depth D-1 is still
    // a plausible cut at depth D.
    KillerTable killers;
    HistoryTable history;

    for (int d = 1; d <= max_depth; ++d) {
        PvTable pv;
        const std::uint64_t nodes_before = result.nodes;
        BranchStats pv_stats;

        if (recorder != nullptr) recorder->begin_iteration(d);
        int score =
            iteration(board, d, result.nodes, pv, killers, history, stop, tt,
                      recorder, pv_stats, alpha, beta);

        if (stop.abort) {
            result.nodes = nodes_before;
            break;
        }

        result.score = score;
        result.completed_depth = d;
        result.pv_stats = pv_stats;
        const std::size_t pv_len = pv.length[0];
        result.best_move =
            pv_len > 0 ? pv.moves[0][0] : *root_moves.begin();
        result.principal_variation.clear();
        result.principal_variation.reserve(pv_len);
        for (std::size_t i = 0; i < pv_len; ++i) {
            result.principal_variation.push_back(pv.moves[0][i]);
        }

        if (is_mate_score(score)) break;
    }

    if (result.completed_depth == 0) {
        result.best_move = *root_moves.begin();
        result.score = evaluate(board);
    }

    // Extend the PV through the TT when the in-search PV was
    // truncated by a prune on the main line. NMP and TT probe-
    // cuts both return without running the child move loop, so
    // their `pv.length` stays zero and the PV copy in the parent
    // ends one ply after the pruning node. The best move at
    // those deeper positions is still in the TT — we just didn't
    // walk through it during search. Replay the known PV on the
    // board, then follow TT best-moves until a miss or a cycle,
    // bounded by the completed depth.
    if (tt != nullptr
        && result.completed_depth > 0
        && !result.principal_variation.empty()) {
        std::size_t made = 0;
        for (const Move& m : result.principal_variation) {
            board.make_move(m);
            ++made;
        }
        std::vector<ZobristKey> seen;
        seen.reserve(static_cast<std::size_t>(result.completed_depth));
        const std::size_t cap =
            static_cast<std::size_t>(result.completed_depth);
        while (result.principal_variation.size() < cap) {
            const ZobristKey k = board.zobrist_key();
            if (std::find(seen.begin(), seen.end(), k) != seen.end()) break;
            seen.push_back(k);
            const TtProbe probe = tt->probe(k);
            if (!probe.hit) break;
            const Move tt_move = probe.entry.move;
            if (tt_move.from == Square::None) break;
            const MoveList legal = MoveGenerator::generate_legal(board);
            bool ok = false;
            for (const Move& lm : legal) {
                if (lm == tt_move) { ok = true; break; }
            }
            if (!ok) break;
            board.make_move(tt_move);
            result.principal_variation.push_back(tt_move);
            ++made;
        }
        for (std::size_t i = 0; i < made; ++i) {
            const Move& m =
                result.principal_variation[made - 1 - i];
            board.unmake_move(m);
        }
    }

    if (tt != nullptr) {
        result.tt_probes = tt->probes() - probes_before;
        result.tt_hits   = tt->hits()   - hits_before;
    }

    result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clk::now() - stop.start);
    return result;
}

} // namespace chesserazade

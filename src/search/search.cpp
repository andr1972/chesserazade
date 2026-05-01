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

#include "board/board_bitboard.hpp"
#include "search/see.hpp"

#include <chesserazade/board.hpp>
#include <chesserazade/evaluator.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/move_generator.hpp>
#include <chesserazade/transposition_table.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    /// Bump with Stockfish-style gravity: the increment shrinks as
    /// the slot approaches `HISTORY_MAX`, so frequently-bumped moves
    /// self-saturate instead of growing without bound. Without this,
    /// a long search lets old hot moves dominate ordering long after
    /// they've stopped being relevant; with it, the table tracks
    /// recency naturally.
    static constexpr std::int32_t HISTORY_MAX = 16384;
    void bump(Color stm, Square from, Square to, int depth) noexcept {
        auto& v = table[static_cast<std::size_t>(stm)]
                       [to_index(from)][to_index(to)];
        const std::int32_t bonus =
            std::min(depth * depth, HISTORY_MAX);
        v += bonus - v * bonus / HISTORY_MAX;
    }
};

/// Counter-move heuristic — for each previous move (`prev_piece`,
/// `prev_to`), remember which quiet move most recently caused a
/// β-cutoff against it. Used as an ordering bonus: that response
/// gets searched ahead of generic-history quiets. Smaller and more
/// targeted than full continuation history; usually +5-10 Elo on
/// engines that didn't have it.
struct CounterMoveTable {
    // [color][prev-piece-type 0..6 = None..King][prev-to 0..63].
    // PieceType is 0..6 (None=0, King=6), so the second dim is 7.
    // Slot 0 (None) is reserved so values index directly without
    // an offset — saves a subtraction in the hot path. The earlier
    // size 6 missed King (index 6) and tripped a debug-build
    // assertion at the end of every game.
    std::array<std::array<std::array<Move, 64>, 7>, 2> table{};

    [[nodiscard]] Move get(Color prev_stm, PieceType prev_piece,
                           Square prev_to) const noexcept {
        return table[static_cast<std::size_t>(prev_stm)]
                    [static_cast<std::size_t>(prev_piece)]
                    [to_index(prev_to)];
    }
    void set(Color prev_stm, PieceType prev_piece,
             Square prev_to, Move m) noexcept {
        table[static_cast<std::size_t>(prev_stm)]
             [static_cast<std::size_t>(prev_piece)]
             [to_index(prev_to)] = m;
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
    bool enable_history = false;
    bool enable_aspiration = false;
    bool enable_pvs = false;
    bool enable_check_ext = false;
    bool enable_nmp_verify = false;
    bool enable_futility = false;
    bool enable_reverse_futility = false;
    SearchLimits::NmpMode nmp_mode = SearchLimits::NmpMode::R3_PlusDepthDiv4;
    SearchLimits::LmrMode lmr_mode = SearchLimits::LmrMode::Constant1;
    /// Transient state for the NMP verification re-search: while
    /// non-zero, NMP is suppressed for `nmp_color` until a node's
    /// `ply` exceeds `nmp_min_ply`. Always 0 outside the verification
    /// scope. See the Step 9 verification block below for details.
    int nmp_min_ply = 0;
    int nmp_color = -1;  // -1 == no constraint
    /// Diagnostic counters — totals across the whole search.
    /// Cheap (one increment per gate) and zero-cost when nothing
    /// reads them; the iterative-deepening loop emits them as
    /// `info string` for analysis runs.
    /// State labels mirror the user's original log markers:
    ///   0 = nmp_rejected   — gate predicate failed (else branch)
    ///   1 = nmp_entered    — gate let the null search run
    ///   2 = nmp_verified   — verify search confirmed the cutoff
    ///   3 = nmp_aborted    — verify search aborted (time/cancel)
    /// Plus intermediate breakdown counts so we can see *why* verify
    /// did/didn't fire when the four-state totals don't match expect.
    std::uint64_t nmp_rejected = 0;
    std::uint64_t nmp_entered = 0;
    std::uint64_t nmp_failed_high = 0;
    std::uint64_t nmp_verify_attempts = 0;
    std::uint64_t nmp_verified = 0;
    std::uint64_t nmp_aborted = 0;
    int contempt_cp = 0;
    std::atomic<bool>* cancel = nullptr;
    std::atomic<std::uint64_t>* progress_nodes = nullptr;
    /// Pre-search game history: zobrist keys of positions reached
    /// before `find_best` was called. Used by repetition detection
    /// in `negamax`.
    std::span<const ZobristKey> position_history{};
    /// Live search-path zobrists, pushed on entry to each
    /// in-search node and popped on exit. A node sees its own
    /// key already in this stack only if the line is a
    /// repetition.
    std::vector<ZobristKey> search_path{};
    /// Per-ply static eval cache used by the `improving` flag —
    /// each negamax frame writes its static_eval here so a child
    /// 2 plies down can compare and decide whether the position is
    /// trending up for our side. Indexed by ply; INT_MIN means
    /// 'not computed yet at that ply' (in-check or before this
    /// search reached that depth).
    std::array<int, static_cast<std::size_t>(Search::MAX_DEPTH)>
        ply_static_eval{};
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
///   ~ 1'000'000 — good capture (SEE ≥ 0 — winning or neutral exchange).
///   ~   900'000 — first killer.
///   ~   850'000 — counter-move (preferred quiet response to prev move).
///   ~   800'000 — second killer.
///   ~   700'000 — bad capture (SEE < 0 — losing exchange chain).
///   ~    50'000 — promotions without capture.
///   0..49'999   — quiet moves, ranked by history heuristic.
[[nodiscard]] int score_move(const Move& m, const Move& tt_move,
                             const KillerTable& killers,
                             const HistoryTable& history,
                             const Move& counter_move,
                             bool capture_is_bad,
                             bool use_history,
                             Color stm,
                             std::size_t ply) noexcept {
    if (m == tt_move && tt_move.from != Square::None) {
        return 10'000'000;
    }
    if (is_capture(m)) {
        // Bad captures fall below killers/counter so legitimate quiet
        // plans get tried first; MVV-LVA still orders within bucket.
        if (capture_is_bad) {
            return 700'000 + mvv_lva(m);
        }
        return 1'000'000 + mvv_lva(m);
    }
    if (m == killers.killers[ply][0]) return 900'000;
    if (m == killers.killers[ply][1]) return 800'000;
    // Counter-move bonus — slots between killer 1 and killer 2 so
    // it competes with the second killer but never displaces the
    // first. The counter table is keyed on the previous move; a
    // null-or-empty `counter_move` (no prev move at root, or the
    // slot has never been bumped) skips this branch trivially.
    if (counter_move.from != Square::None && m == counter_move) {
        return 850'000;
    }
    if (m.kind == MoveKind::Promotion) {
        return 50'000 + piece_value(m.promotion);
    }
    // Quiet move: use history. Clamped below the promotion
    // bucket so a very old, over-accumulated (from,to) can't
    // leapfrog categories. LMR keys on "score == 0" to detect
    // genuinely untested quiets, so we also floor at 0.
    if (!use_history) return 0;
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
/// descending order. When `bb` is non-null, captures are classified
/// as good/bad via full SEE; otherwise all captures share the
/// good-capture bucket (mailbox board, no cheap SEE available).
std::size_t score_and_sort(const Board& board, const MoveList& legal,
                           std::array<ScoredMove, MoveList::CAPACITY>& buf,
                           const Move& tt_move,
                           const KillerTable& killers,
                           const HistoryTable& history,
                           const Move& counter_move,
                           bool use_history,
                           Color stm,
                           std::size_t ply) {
    const auto* bb = dynamic_cast<const BoardBitboard*>(&board);
    const std::size_t n = legal.count;
    for (std::size_t i = 0; i < n; ++i) {
        const Move& m = legal.moves[i];
        const bool bad = bb != nullptr
                         && is_capture(m)
                         && see(*bb, m) < 0;
        buf[i] = {m, score_move(m, tt_move, killers, history,
                                counter_move, bad, use_history,
                                stm, ply)};
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

/// RAII helper: keep `Stop::search_path` consistent with the
/// recursion. Pushes a zobrist on construction, pops on
/// destruction — so every `negamax` return path (early TT cut,
/// quiescence, β-cutoff, normal completion, even an aborted
/// subtree) automatically restores the stack.
struct PathGuard {
    std::vector<ZobristKey>& path;
    PathGuard(std::vector<ZobristKey>& p, ZobristKey k) : path(p) {
        path.push_back(k);
    }
    ~PathGuard() { path.pop_back(); }
    PathGuard(const PathGuard&) = delete;
    PathGuard& operator=(const PathGuard&) = delete;
};

NegamaxResult negamax(Board& board, int depth, int ply, int alpha, int beta,
                      std::uint64_t& nodes, PvTable& pv, KillerTable& killers,
                      HistoryTable& history,
                      CounterMoveTable& counters,
                      Stop& stop, TranspositionTable* tt,
                      TreeRecorder* rec, BranchStats& out_stats,
                      bool allow_null = true,
                      Move prev_move = Move{}) {
    ++nodes;
    out_stats = {};

    // Hard recursion cap. `pv`, `killers`, and `pv.moves[ply+1]`
    // accesses below all assume `ply < PV_SIZE`. Without this
    // guard a position whose search legitimately runs past
    // `MAX_DEPTH` — most easily a perpetual-check sequence with
    // `enable_check_ext` keeping `depth` constant — would index
    // off the end of those fixed-size arrays. Treat `ply ==
    // MAX_DEPTH` as a horizon and bail out with a quiescence
    // verdict (or static eval if quiescence is disabled).
    if (ply >= Search::MAX_DEPTH) {
        if (stop.disable_quiescence) {
            const int e = stop.use_incremental_eval
                ? board.evaluate_incremental()
                : evaluate(board);
            return {e, true};
        }
        return quiesce(board, alpha, beta, nodes, stop, out_stats);
    }

    const std::size_t p = static_cast<std::size_t>(ply);
    pv.length[p] = 0;

    if ((nodes & STOP_CHECK_MASK) == 0 && stop.should_stop(nodes)) {
        return {0, false};
    }

    const ZobristKey key = board.zobrist_key();

    // --- Repetition detection -----------------------------------------
    // 2-fold heuristic: any in-search position whose zobrist matches
    // either the pre-search game history *or* an earlier node on the
    // current search path is a forced draw. Reasoning: whichever side
    // entered the loop can replay it, so the line is at best the
    // draw score for the side hoping to improve. Skip at the root
    // (ply 0) — repetitions of the root itself are detected from
    // ply ≥ 1 once the root key has been pushed onto the path.
    if (ply > 0) {
        // The draw score is `-contempt_cp` from the side-to-
        // move's perspective: a positive contempt makes draws
        // *less* attractive to whoever is on move at this node,
        // which propagates through negamax as "winners avoid /
        // losers seek". When contempt is 0 (the default) the
        // value collapses to plain 0.
        const int draw_score = -stop.contempt_cp;
        for (ZobristKey k : stop.position_history) {
            if (k == key) return {draw_score, true};
        }
        for (ZobristKey k : stop.search_path) {
            if (k == key) return {draw_score, true};
        }
        // 50-move rule (100 half-moves without progress).
        if (board.halfmove_clock() >= 100) {
            return {draw_score, true};
        }
    }

    PathGuard path_guard(stop.search_path, key);

    // --- TT probe -----------------------------------------------------
    // On a probe-cut we reconstruct `exact` from the stored bound.
    // Loop-completed stores use `Exact` (best > α₀) or `Upper`
    // (fail-low, all children visited) — both mean "all direct
    // children were visited" in our sense. A `Lower` store only
    // happens at a β-cutoff, which is precisely `exact=false`.
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
    // Verification gate (SF Stockfish): when verification is in
    // progress for our colour, suppress NMP at plies below
    // `nmp_min_ply`. `nmp_color == -1` means no constraint.
    //
    // Diagnostic counters: per-search totals of how often each NMP
    // path was taken. Read from the iterative-deepening loop and
    // emitted as 'info string' so debug runs can confirm verify
    // actually triggers (it requires depth >= 13 — easy to miss at
    // short TC where IDs never get there). Search-instance state,
    // not global, so concurrent searches don't mix.
    const int us = static_cast<int>(board.side_to_move());
    const bool nmp_allowed_for_side =
        stop.nmp_color < 0 || us != stop.nmp_color || ply >= stop.nmp_min_ply;

    // Hoist static_eval so NMP and futility share one call. Cheap
    // when neither is on (the bool collapses) and a single eval call
    // when at least one is. In-check positions skip — a forced
    // response makes the position's static eval irrelevant for
    // pruning decisions.
    const bool in_check_now =
        MoveGenerator::is_in_check(board, board.side_to_move());
    const bool need_static_eval =
        !in_check_now
        && (stop.nmp_mode != SearchLimits::NmpMode::Off
            || stop.enable_futility);
    const int static_eval = need_static_eval
        ? (stop.use_incremental_eval
            ? board.evaluate_incremental()
            : evaluate(board))
        : 0;
    // Per-ply static_eval cache for the `improving` flag. INT_MIN
    // marks 'unknown' (in-check ancestors, or not reached yet);
    // children at ply+2 read this slot and treat INT_MIN as 'not
    // improving' (conservative — larger futility margin).
    constexpr int EVAL_UNKNOWN = std::numeric_limits<int>::min();
    if (static_cast<std::size_t>(ply)
            < stop.ply_static_eval.size()) {
        stop.ply_static_eval[static_cast<std::size_t>(ply)] =
            need_static_eval ? static_eval : EVAL_UNKNOWN;
    }
    // `improving` = our position got better than 2 plies ago (same
    // side to move). Used to scale futility / NMP thresholds —
    // we can prune more aggressively when the trend is up because
    // a quiet move is unlikely to recover what we lost.
    const int ancestor_eval = (ply >= 2)
        ? stop.ply_static_eval[static_cast<std::size_t>(ply - 2)]
        : EVAL_UNKNOWN;
    const bool improving =
        ancestor_eval != EVAL_UNKNOWN && static_eval > ancestor_eval;

    // --- Reverse futility / static null move pruning ------------------
    // If static_eval already beats β by a depth-scaled margin, the
    // position is so good that even the opponent's best reply on the
    // next move (≈ −margin worst case) can't pull us under β. Return
    // static_eval directly; no recursive search needed. Symmetric
    // mirror of forward futility (which prunes hopeless quiets).
    {
        const bool is_non_pv_here = (beta - alpha == 1);
        if (stop.enable_reverse_futility
            && is_non_pv_here
            && !stop.disable_alpha_beta
            && !in_check_now
            && ply > 0
            && depth >= 1 && depth <= 6
            && need_static_eval) {
            // Margin grows linearly with depth; smaller when
            // improving (we trust the eval more). 80 cp/depth
            // matches the rough scale of forward-futility margins.
            const int margin = 80 * depth - (improving ? 30 : 0);
            if (static_eval - margin >= beta
                && static_eval < Search::MATE_SCORE - 1000) {
                out_stats = {};
                return {static_eval, false};
            }
        }
    }

    if (allow_null
        && stop.nmp_mode != SearchLimits::NmpMode::Off
        && nmp_allowed_for_side
        && !stop.disable_alpha_beta
        && ply > 0
        && depth >= 3
        && !in_check_now
        && has_non_pawn_material(board, board.side_to_move())) {
        ++stop.nmp_entered;
        constexpr int NMP_VERIFY_MIN_DEPTH = 13;
        if (static_eval >= beta) {
            // Reduction formula picked by --nmp-mode. The depth-
            // scaled variants prune harder at deep searches, where
            // the null move is a stronger signal that we have
            // breathing room.
            int NMP_R = 4;
            switch (stop.nmp_mode) {
                case SearchLimits::NmpMode::Off:
                    break;  // unreachable — guarded above
                case SearchLimits::NmpMode::R4:
                    NMP_R = 4;
                    break;
                case SearchLimits::NmpMode::R3_PlusDepthDiv3:
                    NMP_R = 3 + depth / 3;
                    break;
                case SearchLimits::NmpMode::R4_PlusDepthDiv4:
                    NMP_R = 4 + depth / 4;
                    break;
                case SearchLimits::NmpMode::R3_PlusDepthDiv4:
                    NMP_R = 3 + depth / 4;
                    break;
                case SearchLimits::NmpMode::R2_PlusDepthDiv3:
                    NMP_R = 2 + depth / 3;
                    break;
            }
            board.make_null_move();
            BranchStats null_stats;
            const NegamaxResult nr = negamax(
                board, depth - NMP_R, ply + 1,
                -beta, -beta + 1,
                nodes, pv, killers, history, counters, stop, tt,
                /*rec=*/nullptr, null_stats,
                /*allow_null=*/false);
            board.unmake_null_move();

            if (stop.abort) return {0, false};

            const int null_score = -nr.score;
            if (null_score >= beta) {
                ++stop.nmp_failed_high;
                // Verification re-search (SF classical Step 9): at
                // high depths, when not already inside a verification
                // scope, repeat the same reduced-depth search but
                // for *our* side, with NMP disabled for us until
                // `ply + 3·(depth−R)/4`. If verification confirms
                // the cutoff (v ≥ β), accept it; otherwise fall
                // through and search normally. Catches zugzwang
                // positions where the opponent's null move was
                // misleading. SF threshold is depth ≥ 13.
                const bool need_verify =
                    stop.enable_nmp_verify
                    && stop.nmp_min_ply == 0
                    && depth >= NMP_VERIFY_MIN_DEPTH;
                if (!need_verify) {
                    return {null_score, false};
                }
                ++stop.nmp_verify_attempts;
                stop.nmp_min_ply = ply + 3 * (depth - NMP_R) / 4;
                stop.nmp_color = us;
                BranchStats verify_stats;
                const NegamaxResult vr = negamax(
                    board, depth - NMP_R, ply,
                    beta - 1, beta,
                    nodes, pv, killers, history, counters, stop, tt,
                    /*rec=*/nullptr, verify_stats,
                    /*allow_null=*/true);
                stop.nmp_min_ply = 0;
                stop.nmp_color = -1;

                if (stop.abort) {
                    ++stop.nmp_aborted;
                    return {0, false};
                }
                if (vr.score >= beta) {
                    ++stop.nmp_verified;
                    return {null_score, false};
                }
                // Verification failed — fall through to normal search.
            }
        }
    } else {
        ++stop.nmp_rejected;
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
    // Look up the counter-move recorded against `prev_move`. At the
    // root or after a null-move shadow search prev_move is empty;
    // counter_move stays empty in that case and the bonus simply
    // doesn't fire.
    Move counter_move{};
    if (prev_move.from != Square::None) {
        // Previous mover was the *opponent* of the current side.
        const Color prev_stm = (board.side_to_move() == Color::White)
            ? Color::Black : Color::White;
        counter_move = counters.get(prev_stm,
                                    prev_move.moved_piece.type,
                                    prev_move.to);
    }
    std::array<ScoredMove, MoveList::CAPACITY> buf;
    const std::size_t n =
        score_and_sort(board, legal, buf, tt_move, killers, history,
                       counter_move,
                       stop.enable_history,
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

    // Non-PV-node detection for LMP (and later pruning work).
    // `beta - alpha == 1` at entry means the caller ran a
    // zero-width probe (PVS non-first move or a null-move
    // shadow search); we only have to prove "does not beat α".
    const bool is_non_pv = (beta - original_alpha == 1);

    for (std::size_t i = 0; i < n; ++i) {
        const Move& m = buf[i].move;
        const Color mover = board.side_to_move();

        // --- LMP: skip late quiet moves in non-PV-nodes ---
        // Classical formula: at depth d ≤ 3, keep the first
        // `3 + d*d` scored moves (4 at d=1, 7 at d=2, 12 at
        // d=3) and drop the rest outright — no probe, no
        // re-search. PV-nodes need every move because they
        // aim at the exact best; non-PV-nodes only prove a
        // bound, so the speculative cut is safe. Quiet bucket
        // check (score < 50'000) spares captures, killers,
        // promotions and the TT move. Ties on-and-off with
        // PVS since without PVS there are essentially no
        // non-PV-nodes to prune on.
        if (stop.enable_pvs
            && is_non_pv
            && !stop.disable_alpha_beta
            && depth <= 3
            && buf[i].score < 50'000
            && i >= static_cast<std::size_t>(3 + depth * depth)) {
            continue;
        }

        // --- Forward futility pruning ---
        // For quiet moves at low depth in non-PV-nodes, skip if the
        // static eval plus a depth-scaled margin still can't reach α.
        // Reasoning: the move is unlikely to swing eval by more than
        // `margin` cp, so even a "lucky" outcome wouldn't beat the
        // current α-bound — no point paying for the search. Captures
        // / promotions / TT-move / killers (score ≥ 50'000) are
        // exempt because their material swing can exceed the margin.
        // Skipped in check (no static eval available) and at the root
        // (ply == 0) where exact bounds matter.
        if (stop.enable_futility
            && is_non_pv
            && !stop.disable_alpha_beta
            && !in_check_now
            && ply > 0
            && depth >= 1 && depth <= 5
            && buf[i].score < 50'000) {
            constexpr std::array<int, 6> FUTILITY_MARGINS = {
                0, 100, 175, 270, 380, 500};
            // `improving` shrinks the margin so we prune harder when
            // the trend is up. The shrink is depth-scaled so deep
            // searches (where the wiggle room matters more) keep
            // most of their margin; depth=1 cut by 30, depth=5 by ~75.
            const int base = FUTILITY_MARGINS[
                static_cast<std::size_t>(depth)];
            const int margin = improving ? base - 30 * depth / 2
                                         : base;
            if (static_eval + margin <= alpha) {
                continue;
            }
        }

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
        // Probe post-move "is the opponent in check?" whenever
        // either the recorder needs it (for the Checks columns)
        // or check extensions need it (to bump search depth on
        // checking moves).
        if (rec != nullptr || stop.enable_check_ext) {
            const Color them = board.side_to_move();
            if (MoveGenerator::is_in_check(board, them)) {
                gives_check = true;
                if (rec != nullptr) {
                    if (mover == Color::White) delta.checks_white = 1;
                    else                       delta.checks_black = 1;
                }
            }
        }
        const int ext = (stop.enable_check_ext && gives_check) ? 1 : 0;

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

        // --- LMR / PVS: cheap probe, re-search when it fires ---
        // LMR candidate: "late" quiet move — index ≥ 3 (after
        // TT / captures / killers) AND score below the
        // promotion bucket (< 50'000). That covers every quiet
        // whether or not history has bumped it, while still
        // excluding captures (≥1'000'000), killers (800k/900k),
        // promotions (≥50'000) and the TT move (10'000'000).
        //
        // PVS candidate: every non-first move, provided α-β is
        // on and we're not at a root_full_window root. The
        // first move is always searched with the full window
        // because it *is* the (current) principal variation.
        //
        // When either fires the probe runs with the recorder
        // detached, identical to null-move pruning. If the
        // probe score beats α — meaning the reduction and/or
        // the zero-width window may have hidden a better move
        // — we re-search at full depth with the full window
        // and the recorder attached, so the tree view sees the
        // confirmed subtree exactly once.
        const bool lmr_candidate =
               stop.enable_lmr
            && stop.lmr_mode != SearchLimits::LmrMode::Off
            && !stop.disable_alpha_beta
            && depth >= 3
            && i >= 3
            && buf[i].score < 50'000;
        const bool pvs_probe =
               stop.enable_pvs
            && !stop.disable_alpha_beta
            && !(stop.root_full_window && ply == 0)
            && i > 0;

        // Reduction policy. `lmr_candidate` already vouched for
        // 'late quiet move at non-leaf depth'; the mode picks how
        // far to shorten. Re-search on probe-fail-high keeps us
        // safe — over-reducing only costs the saved time, never
        // correctness. Floor at 0 means 'no reduction'; we never
        // negative-reduce.
        int R = 0;
        if (lmr_candidate) {
            switch (stop.lmr_mode) {
                case SearchLimits::LmrMode::Off:
                    break;  // unreachable — guarded above
                case SearchLimits::LmrMode::Constant1:
                    R = 1;
                    break;
                case SearchLimits::LmrMode::LogDepthLogIndex: {
                    const double v =
                        std::log(double(depth))
                        * std::log(double(i + 1)) / 2.0;
                    R = std::max(0, int(v));
                    break;
                }
                case SearchLimits::LmrMode::DepthDiv4LogIdxHalf: {
                    const double v =
                        (double(depth) / 4.0)
                        * std::log(double(i + 1)) / 2.0;
                    R = std::max(0, int(v));
                    break;
                }
                case SearchLimits::LmrMode::DepthDiv4LogIndex: {
                    const double v =
                        (double(depth) / 4.0)
                        * std::log(double(i + 1));
                    R = std::max(0, int(v));
                    break;
                }
            }
            // Soften the log-based policies (option B, raised from
            // depth-R≥4 to ≥5): cap R so depth-R ≥ 5 — guarantees
            // the LMR'd child still searches deep enough that the
            // 'good move' which was rank 1 at depth 5 (but rank 4
            // at depth 4 in the test position) doesn't get pruned.
            // Constant1 untouched (already respects this).
            if (stop.lmr_mode != SearchLimits::LmrMode::Constant1
                && stop.lmr_mode != SearchLimits::LmrMode::Off) {
                const int max_R = depth - 5;
                if (max_R < 0) R = 0;
                else if (R > max_R) R = max_R;
            }
        }
        int probe_alpha = recurse_alpha;
        int probe_beta  = recurse_beta;
        if (pvs_probe) {
            probe_alpha = -alpha - 1;
            probe_beta  = -alpha;
        }
        const bool detach_rec_for_probe = (R > 0) || pvs_probe;

        BranchStats child_stats;
        NegamaxResult child =
            negamax(board, depth - 1 - R + ext, child_ply,
                    probe_alpha, probe_beta,
                    nodes, pv, killers, history, counters, stop, tt,
                    detach_rec_for_probe ? nullptr : rec,
                    child_stats, /*allow_null=*/true, m);
        int score = -child.score;
        if (detach_rec_for_probe && !stop.abort && score > alpha) {
            child_stats = {};
            child = negamax(board, depth - 1 + ext, child_ply,
                            recurse_alpha, recurse_beta,
                            nodes, pv, killers, history, counters, stop, tt,
                            rec, child_stats, /*allow_null=*/true, m);
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
            if (stop.enable_history) {
                remember_history(history, mover, m, depth);
            }
            // Record counter-move only for genuinely quiet cutoffs.
            // Captures already get strong ordering from MVV-LVA so
            // the counter table stays focused on the harder problem
            // of "which quiet to try after this opponent move".
            if (prev_move.from != Square::None
                && !is_capture(m)
                && m.kind != MoveKind::Promotion) {
                const Color prev_stm = (mover == Color::White)
                    ? Color::Black : Color::White;
                counters.set(prev_stm,
                             prev_move.moved_piece.type,
                             prev_move.to, m);
            }
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
              CounterMoveTable& counters,
              Stop& stop, TranspositionTable* tt,
              TreeRecorder* rec, BranchStats& out_stats,
              int alpha, int beta) {
    const NegamaxResult r =
        negamax(board, depth, /*ply=*/0,
                alpha, beta,
                nodes, pv, killers, history, counters, stop, tt,
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
        limits.enable_history,
        limits.enable_aspiration,
        limits.enable_pvs,
        limits.enable_check_ext,
        limits.enable_nmp_verify,
        limits.enable_futility,
        limits.enable_reverse_futility,
        limits.nmp_mode,
        limits.lmr_mode,
        0,    // nmp_min_ply
        -1,   // nmp_color (no constraint)
        0, 0, 0, 0, 0, 0,  // nmp_{rejected,entered,failed_high,verify_attempts,verified,aborted}
        limits.contempt_cp,
        limits.cancel,
        limits.progress_nodes,
        limits.position_history,
        {},     // search_path: starts empty, grows during recursion
        false,  // abort
    };
    // Reserve up front so the per-node push/pop never reallocates
    // (which would invalidate the inline span used by the recursion).
    stop.search_path.reserve(static_cast<std::size_t>(MAX_DEPTH) + 1);

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
    CounterMoveTable counters;

    // Aspiration windows: initial half-width around the
    // previous iteration's score. 50 cp is comfortable margin
    // over typical iter-to-iter score drift; anything wider
    // would rarely fail but lose the ordering benefit.
    constexpr int ASP_INITIAL_HALF = 50;
    constexpr int ASP_MIN_DEPTH    = 4;

    for (int d = 1; d <= max_depth; ++d) {
        PvTable pv;
        const std::uint64_t nodes_before = result.nodes;
        BranchStats pv_stats;

        int asp_a = alpha;
        int asp_b = beta;
        int asp_half = ASP_INITIAL_HALF;
        const bool use_asp =
               stop.enable_aspiration
            && d >= ASP_MIN_DEPTH
            && result.completed_depth > 0
            && !is_mate_score(result.score)
            && !stop.disable_alpha_beta;
        if (use_asp) {
            asp_a = std::max(alpha, result.score - asp_half);
            asp_b = std::min(beta,  result.score + asp_half);
        }

        int score = 0;
        while (true) {
            if (recorder != nullptr) recorder->begin_iteration(d);
            score =
                iteration(board, d, result.nodes, pv, killers, history,
                          counters, stop, tt, recorder, pv_stats,
                          asp_a, asp_b);
            if (stop.abort) break;

            // Fail-low / fail-high check only matters when the
            // tested bound is narrower than the outer one —
            // otherwise the score is a real value, not a bound.
            const bool fail_low  = score <= asp_a && asp_a > alpha;
            const bool fail_high = score >= asp_b && asp_b < beta;
            if (!fail_low && !fail_high) break;

            asp_half *= 4;
            if (asp_half > 2 * Search::INF_SCORE) {
                asp_a = alpha;
                asp_b = beta;
            } else if (fail_low) {
                asp_a = std::max(alpha, result.score - asp_half);
            } else {
                asp_b = std::min(beta,  result.score + asp_half);
            }
        }

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

    result.nmp_rejected        = stop.nmp_rejected;
    result.nmp_entered         = stop.nmp_entered;
    result.nmp_failed_high     = stop.nmp_failed_high;
    result.nmp_verify_attempts = stop.nmp_verify_attempts;
    result.nmp_verified        = stop.nmp_verified;
    result.nmp_aborted         = stop.nmp_aborted;

    result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clk::now() - stop.start);
    return result;
}

} // namespace chesserazade

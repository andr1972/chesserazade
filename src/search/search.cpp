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

/// Two killer-move slots per ply. A "killer" is a quiet move that
/// caused a beta cutoff at the same ply of a sibling node — the
/// heuristic bet is that it will cut again here. We keep only the
/// two most recent and never duplicate the slot-0 killer into
/// slot-1.
struct KillerTable {
    std::array<std::array<Move, 2>, PV_SIZE> killers{};
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
///              0 — plain quiet moves.
[[nodiscard]] int score_move(const Move& m, const Move& tt_move,
                             const KillerTable& killers,
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
    return 0;
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
                           std::size_t ply) noexcept {
    const std::size_t n = legal.count;
    for (std::size_t i = 0; i < n; ++i) {
        const Move& m = legal.moves[i];
        buf[i] = {m, score_move(m, tt_move, killers, ply)};
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

int quiesce(Board& board, int alpha, int beta,
            std::uint64_t& nodes, Stop& stop, BranchStats& out_stats);

int quiesce(Board& board, int alpha, int beta,
            std::uint64_t& nodes, Stop& stop, BranchStats& out_stats) {
    ++nodes;
    out_stats = {};
    if ((nodes & STOP_CHECK_MASK) == 0 && stop.should_stop(nodes)) {
        return 0;
    }

    const int stand_pat = stop.use_incremental_eval
        ? board.evaluate_incremental()
        : evaluate(board);
    // Fail-soft: return the actual value we know, not just the
    // window bound. Keeps the correctness of α-β (the caller
    // only needs "≥ beta" for a cut) while letting the tree
    // view show the true magnitude of bad moves.
    if (!stop.disable_alpha_beta && stand_pat >= beta) return stand_pat;
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
        const int score =
            -quiesce(board, -beta, -alpha, nodes, stop, child_stats);
        board.unmake_move(m);
        if (stop.abort) return 0;

        const BranchStats combined = combine(delta, child_stats);
        if (score > best_score) {
            best_score = score;
            best_stats = combined;
        }
        if (score > alpha) alpha = score;
        if (!stop.disable_alpha_beta && score >= beta) {
            out_stats = combined;
            return score;
        }
    }
    out_stats = best_stats;
    return best_score;
}

// ---------------------------------------------------------------------------
// Main negamax
// ---------------------------------------------------------------------------

int negamax(Board& board, int depth, int ply, int alpha, int beta,
            std::uint64_t& nodes, PvTable& pv, KillerTable& killers,
            Stop& stop, TranspositionTable* tt,
            TreeRecorder* rec, BranchStats& out_stats) {
    ++nodes;
    out_stats = {};

    const std::size_t p = static_cast<std::size_t>(ply);
    pv.length[p] = 0;

    if ((nodes & STOP_CHECK_MASK) == 0 && stop.should_stop(nodes)) {
        return 0;
    }

    // --- TT probe -----------------------------------------------------
    const ZobristKey key = board.zobrist_key();
    Move tt_move{};
    if (tt != nullptr) {
        const TtProbe probe = tt->probe(key);
        if (probe.hit) {
            tt_move = probe.entry.move;
            if (ply > 0
                && static_cast<int>(probe.entry.depth) >= depth) {
                const int s = from_tt_score(probe.entry.score, ply);
                switch (probe.entry.bound()) {
                    case TtBound::Exact: return s;
                    case TtBound::Lower:
                        if (!stop.disable_alpha_beta && s >= beta)
                            return s;
                        break;
                    case TtBound::Upper:
                        if (!stop.disable_alpha_beta && s <= alpha)
                            return s;
                        break;
                    case TtBound::None:  break;
                }
            }
        }
    }

    // --- Horizon: quiesce ---------------------------------------------
    if (depth == 0) {
        if (stop.disable_quiescence) {
            // Raw static eval — horizon effect visible. User
            // opt-in to see what "pure depth-N minimax" gives.
            return stop.use_incremental_eval
                ? board.evaluate_incremental()
                : evaluate(board);
        }
        // Pass out_stats through so quiescence captures /
        // recaptures are reflected in the parent's tree view
        // — otherwise the Captures columns under-report (a
        // rook-takes-queen with quiesce-recapture would show
        // W:900, B:0 while the score is 0).
        return quiesce(board, alpha, beta, nodes, stop, out_stats);
    }

    // --- Legality + terminal ------------------------------------------
    const MoveList legal = MoveGenerator::generate_legal(board);
    if (legal.empty()) {
        if (MoveGenerator::is_in_check(board, board.side_to_move())) {
            return -Search::MATE_SCORE + ply;
        }
        return 0; // stalemate
    }

    // --- Move ordering ------------------------------------------------
    std::array<ScoredMove, MoveList::CAPACITY> buf;
    const std::size_t n =
        score_and_sort(legal, buf, tt_move, killers, p);

    const int original_alpha = alpha;
    Move best_move{};
    BranchStats best_stats;
    // `best_score` tracks the best result actually observed in
    // this subtree, independent of the starting α. Returning
    // it (fail-soft) preserves the true magnitude for fail-
    // low nodes: a move that's clearly worse than the current
    // best at root no longer masquerades as the alpha bound.
    int best_score = -Search::INF_SCORE;

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

        BranchStats child_stats;
        const int score =
            -negamax(board, depth - 1, child_ply,
                     recurse_alpha, recurse_beta,
                     nodes, pv, killers, stop, tt,
                     rec, child_stats);
        board.unmake_move(m);

        const BranchStats combined = combine(delta, child_stats);

        if (report_children) {
            const bool caused_cutoff = (score >= beta);
            rec->leave(child_ply, score, caused_cutoff, combined,
                       /*remaining_depth=*/depth - 1,
                       child_alpha, child_beta,
                       /*subtree_nodes=*/nodes - nodes_before,
                       gives_check);
        }

        if (stop.abort) return 0;

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
            if (tt != nullptr) {
                tt->store(key, depth, to_tt_score(score, ply),
                          TtBound::Lower, m);
            }
            out_stats = combined;
            return score; // fail-soft fail-high
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
    return best_score; // fail-soft fail-low
}

int iteration(Board& board, int depth, std::uint64_t& nodes,
              PvTable& pv, KillerTable& killers,
              Stop& stop, TranspositionTable* tt,
              TreeRecorder* rec, BranchStats& out_stats,
              int alpha, int beta) {
    return negamax(board, depth, /*ply=*/0,
                   alpha, beta,
                   nodes, pv, killers, stop, tt,
                   rec, out_stats);
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

    for (int d = 1; d <= max_depth; ++d) {
        PvTable pv;
        const std::uint64_t nodes_before = result.nodes;
        BranchStats pv_stats;

        if (recorder != nullptr) recorder->begin_iteration(d);
        int score =
            iteration(board, d, result.nodes, pv, killers, stop, tt,
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

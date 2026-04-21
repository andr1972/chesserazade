/// Alpha-beta negamax with iterative deepening.
///
/// The core recursion (`negamax`) is the classical textbook form
/// with a single pair of bounds `[alpha, beta]`:
///
/// ```
/// negamax(board, depth, ply, alpha, beta):
///     if depth == 0:             return evaluate(board)
///     moves = legal_moves(board)
///     if moves is empty:
///         return in_check ? -MATE + ply : 0
///     for m in moves:
///         make(m); s = -negamax(..., -beta, -alpha); unmake(m)
///         if s >= beta:          return beta      # beta cutoff (fail-hard)
///         if s > alpha:          alpha = s; ... record PV ...
///     return alpha
/// ```
///
/// We use the **fail-hard** alpha-beta variant: returned scores
/// are always clamped to `[alpha, beta]`. The alternative
/// (fail-soft) is slightly more informative but complicates the
/// code for no educational benefit at this stage.
///
/// Iterative deepening (`find_best`) calls `negamax` once per
/// depth from 1 to `limits.max_depth`. The best move from the
/// last *completed* iteration is kept, so that when a time or
/// node limit interrupts depth N+1, the result still reflects
/// the fully-searched depth N.
///
/// Stop checks: the helper `Stop` caches the start time and the
/// budgets, and is polled once every `STOP_CHECK_MASK + 1` nodes
/// rather than on every node. Cheaper than a chrono call at each
/// visit, and still responsive to a time limit within ms.
#include <chesserazade/search.hpp>

#include <chesserazade/board.hpp>
#include <chesserazade/evaluator.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/move_generator.hpp>
#include <chesserazade/transposition_table.hpp>

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

/// Budget enforcement. A single `abort` flag short-circuits the
/// recursion; we check the clock / node count only on a power-of-
/// two boundary to keep the per-node overhead negligible.
struct Stop {
    clk::time_point start;
    std::chrono::milliseconds time_budget;
    std::uint64_t node_budget;
    bool abort = false;

    /// Poll the budget. Call occasionally, not on every node.
    /// Returns true once the search should bail out.
    bool should_stop(std::uint64_t nodes_so_far) noexcept {
        if (abort) return true;
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

/// Only poll the clock / node budget on a 1-in-2048 cadence. A
/// single perft benchmark shows this is ~0.1% overhead while
/// staying within a few milliseconds of the requested time
/// budget.
constexpr std::uint64_t STOP_CHECK_MASK = 2047;

/// Mate scores are stored ply-relative so that a cached entry
/// found at a different root distance still means the right
/// thing. On *store*: shift a mate-against-us score away from 0
/// by `ply` plies (so it becomes a larger-magnitude "mate soon
/// from this node"); on *probe*: shift back by the current ply.
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

int negamax(Board& board, int depth, int ply, int alpha, int beta,
            std::uint64_t& nodes, PvTable& pv, Stop& stop,
            TranspositionTable* tt) {
    ++nodes;

    const std::size_t p = static_cast<std::size_t>(ply);
    pv.length[p] = 0;

    if ((nodes & STOP_CHECK_MASK) == 0 && stop.should_stop(nodes)) {
        return 0; // value discarded by the driver when abort is set
    }

    // --- TT probe -----------------------------------------------------
    // Only cut off at non-root nodes (ply > 0). At the root we
    // always want a completed search so we can return a full PV;
    // a TT cutoff at ply 0 would leave `pv` empty.
    const ZobristKey key = board.zobrist_key();
    if (tt != nullptr && ply > 0) {
        const TtProbe probe = tt->probe(key);
        if (probe.hit && static_cast<int>(probe.entry.depth) >= depth) {
            const int s = from_tt_score(probe.entry.score, ply);
            switch (probe.entry.bound()) {
                case TtBound::Exact: return s;
                case TtBound::Lower: if (s >= beta)  return s; break;
                case TtBound::Upper: if (s <= alpha) return s; break;
                case TtBound::None:  break;
            }
        }
    }

    if (depth == 0) {
        return evaluate(board);
    }

    const MoveList legal = MoveGenerator::generate_legal(board);
    if (legal.empty()) {
        if (MoveGenerator::is_in_check(board, board.side_to_move())) {
            return -Search::MATE_SCORE + ply;
        }
        return 0; // stalemate
    }

    const int original_alpha = alpha;
    Move best_move{};

    for (const Move& m : legal) {
        board.make_move(m);
        const int score =
            -negamax(board, depth - 1, ply + 1, -beta, -alpha, nodes, pv, stop, tt);
        board.unmake_move(m);

        if (stop.abort) {
            return 0;
        }

        if (score >= beta) {
            // Beta cutoff (fail-hard). The opponent would never
            // let us reach this line — we can safely prune the
            // rest. Store as a LOWER bound: the true value is
            // at least `beta`, possibly higher.
            if (tt != nullptr) {
                tt->store(key, depth, to_tt_score(beta, ply),
                          TtBound::Lower, m);
            }
            return beta;
        }
        if (score > alpha) {
            alpha = score;
            best_move = m;
            pv.moves[p][0] = m;
            const std::size_t child_len = pv.length[p + 1];
            for (std::size_t i = 0; i < child_len; ++i) {
                pv.moves[p][i + 1] = pv.moves[p + 1][i];
            }
            pv.length[p] = 1 + child_len;
        }
    }

    // --- TT store -----------------------------------------------------
    // Exact if we raised alpha at least once (best_move is set);
    // Upper bound (fail-low) if every move returned at or below
    // the original alpha.
    if (tt != nullptr) {
        const TtBound bound = (alpha > original_alpha)
                                  ? TtBound::Exact
                                  : TtBound::Upper;
        tt->store(key, depth, to_tt_score(alpha, ply), bound, best_move);
    }

    return alpha;
}

/// Run one full-width iteration at the given depth. Returns the
/// root score; fills `pv` with the principal variation. If the
/// stop flag fires during the iteration, the return value is not
/// meaningful and the caller must discard it.
int iteration(Board& board, int depth, std::uint64_t& nodes,
              PvTable& pv, Stop& stop, TranspositionTable* tt) {
    return negamax(board, depth, /*ply=*/0,
                   -Search::INF_SCORE, Search::INF_SCORE,
                   nodes, pv, stop, tt);
}

} // namespace

SearchResult Search::find_best(Board& board, int depth) {
    SearchLimits l;
    l.max_depth = depth;
    return find_best(board, l, nullptr);
}

SearchResult Search::find_best(Board& board, const SearchLimits& limits) {
    return find_best(board, limits, nullptr);
}

SearchResult Search::find_best(Board& board, const SearchLimits& limits,
                               TranspositionTable* tt) {
    SearchResult result;

    int max_depth = limits.max_depth;
    if (max_depth < 0) max_depth = 0;
    if (max_depth > MAX_DEPTH) max_depth = MAX_DEPTH;

    Stop stop{
        clk::now(),
        limits.time_budget,
        limits.node_budget,
        false,
    };

    // Root terminal: no legal moves means the caller handed us a
    // mated or stalemated position. No search to do.
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

    // Depth-0: no iterations to run. Pick the first legal move
    // and report the static eval so the CLI has something to
    // display.
    if (max_depth == 0) {
        result.best_move = *root_moves.begin();
        result.score = evaluate(board);
        result.nodes = 1;
        result.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                clk::now() - stop.start);
        return result;
    }

    // Bump the TT age for this new root search so stored
    // entries from the previous call become replaceable.
    if (tt != nullptr) tt->new_search();

    const std::uint64_t probes_before = tt != nullptr ? tt->probes() : 0;
    const std::uint64_t hits_before   = tt != nullptr ? tt->hits()   : 0;

    // Iterative deepening. We keep the best move and PV from the
    // last *completed* iteration; if the budget cuts an iteration
    // mid-flight we discard its (possibly lying) partial result.
    for (int d = 1; d <= max_depth; ++d) {
        PvTable pv;
        const std::uint64_t nodes_before = result.nodes;
        const int score = iteration(board, d, result.nodes, pv, stop, tt);

        if (stop.abort) {
            // Roll the partially-accumulated node count back so
            // the reported count matches the last completed depth.
            result.nodes = nodes_before;
            break;
        }

        result.score = score;
        result.completed_depth = d;
        const std::size_t pv_len = pv.length[0];
        result.best_move = pv_len > 0 ? pv.moves[0][0] : *root_moves.begin();
        result.principal_variation.clear();
        result.principal_variation.reserve(pv_len);
        for (std::size_t i = 0; i < pv_len; ++i) {
            result.principal_variation.push_back(pv.moves[0][i]);
        }

        // Short-circuit: a found mate at this depth cannot be
        // beaten by deeper searching (faster mates would have
        // been found at shallower depths already).
        if (is_mate_score(score)) {
            break;
        }
    }

    // If nothing completed (e.g. a time budget of 0 ms), fall
    // back to a playable move + static eval.
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

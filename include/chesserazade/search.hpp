/// Search — alpha-beta negamax with iterative deepening and
/// optional time / node limits.
///
/// This is the 0.6 search. The algorithm is still a recognizable
/// textbook negamax, but with two classical additions:
///
///   * **Alpha-beta pruning.** At each node we carry a `[alpha,
///     beta]` window. A child score that matches or beats `beta`
///     (a "beta cutoff") proves the opponent will never allow
///     this line, so we stop exploring siblings. Result: the
///     same best move as plain negamax, but the tree can be up
///     to an order of magnitude smaller.
///     See https://www.chessprogramming.org/Alpha-Beta
///
///   * **Iterative deepening.** Rather than jumping straight to
///     depth N, we call the search at depth 1, then 2, then 3,
///     …, up to the requested depth or until a limit fires.
///     The cost of the shallow iterations is dwarfed by the
///     final one, and we get an anytime property: if the search
///     is cut off, the best move from the last *completed* depth
///     is still available.
///     See https://www.chessprogramming.org/Iterative_Deepening
///
/// Scoring is unchanged from 0.5 (mate = `±(MATE_SCORE - ply)`,
/// stalemate = 0, evaluator otherwise). See `evaluator.hpp` for
/// the static eval, and `is_mate_score` / `plies_to_mate` below
/// for the interpretation helpers.
///
/// Not yet here (per HANDOFF): transposition table (0.7), move
/// ordering beyond trivial (0.8), quiescence (0.8).
#pragma once

#include <chesserazade/move.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

namespace chesserazade {

class Board;
class TranspositionTable;

/// Optional limits on a search. Any combination may be set; the
/// search stops at the earliest trigger. A default-constructed
/// `Limits` is effectively unlimited (depth cap is the only hard
/// stop — without it the search would recurse forever on a loop
/// position).
struct SearchLimits {
    /// Maximum depth in plies. The search iterates up to (and
    /// including) this depth. Clamped to `Search::MAX_DEPTH`.
    int max_depth = 32;

    /// Wall-clock budget. Zero means "no time limit". Checked
    /// periodically during the search (not on every node, to
    /// keep the check cheap).
    std::chrono::milliseconds time_budget{0};

    /// Soft cap on visited nodes. Zero means "no node limit".
    /// Checked on the same cadence as `time_budget`.
    std::uint64_t node_budget = 0;
};

struct SearchResult {
    /// Best move at the root. Default-constructed if the position
    /// has no legal moves (mate or stalemate at the root itself).
    Move best_move{};

    /// Score in centipawns, from the side-to-move's perspective.
    /// Positive = good for the side to move. May be a mate score;
    /// use `Search::is_mate_score` / `Search::plies_to_mate` to
    /// interpret.
    int score = 0;

    /// The main line starting from `best_move`, one entry per
    /// ply. `principal_variation[0] == best_move`.
    std::vector<Move> principal_variation;

    /// Number of nodes visited. Under iterative deepening this
    /// is the *cumulative* count across all depths searched.
    std::uint64_t nodes = 0;

    /// Wall-clock time for the whole search (all depths).
    std::chrono::milliseconds elapsed{0};

    /// Deepest depth that *completed* — the result reflects this
    /// depth, not necessarily `limits.max_depth`. When a time or
    /// node limit cuts in, `completed_depth` is one less than
    /// the iteration we abandoned, but `best_move` is still the
    /// best move from the last fully completed iteration.
    int completed_depth = 0;

    /// Transposition-table statistics for this search. When no
    /// TT is supplied to `find_best`, these remain zero.
    std::uint64_t tt_probes = 0;
    std::uint64_t tt_hits   = 0;
};

class Search {
public:
    /// The "you are mated at this ply" sentinel. Fits in 16 bits.
    static constexpr int MATE_SCORE = 32000;

    /// Any positional score the evaluator produces must stay
    /// strictly below this. Used as the initial ±∞ of the
    /// alpha-beta window at the root.
    static constexpr int INF_SCORE = 32001;

    /// Maximum search depth this engine will accept. Bounds the
    /// on-stack triangular PV table.
    static constexpr int MAX_DEPTH = 32;

    /// Find the best move in `board`, iteratively deepening up
    /// to `depth`. `board` is mutated during the search and
    /// restored on return. Equivalent to calling the `Limits`
    /// overload with `{ depth, 0ms, 0 }`.
    [[nodiscard]] static SearchResult find_best(Board& board, int depth);

    /// Find the best move honoring the given limits. The search
    /// runs iterative deepening from depth 1 up to
    /// `limits.max_depth`; it stops before the next iteration
    /// when the time or node budget has been exhausted.
    [[nodiscard]] static SearchResult find_best(Board& board,
                                                const SearchLimits& limits);

    /// Find the best move using the given transposition table.
    /// The table is owned by the caller so it can be re-used
    /// across calls (iterative deepening already re-uses it
    /// across depths, but a REPL session might want to share one
    /// across user moves). Pass `nullptr` to search without a
    /// TT — identical behavior to the non-TT overloads.
    [[nodiscard]] static SearchResult find_best(Board& board,
                                                const SearchLimits& limits,
                                                TranspositionTable* tt);

    /// True if `score` encodes a forced mate (won or lost).
    [[nodiscard]] static constexpr bool is_mate_score(int score) noexcept {
        return score > MATE_SCORE - 1000 || score < -(MATE_SCORE - 1000);
    }

    /// If `score` is a mate score, return the signed number of
    /// plies to mate (positive = we win, negative = we lose).
    /// Returns 0 for non-mate scores.
    [[nodiscard]] static constexpr int plies_to_mate(int score) noexcept {
        if (!is_mate_score(score)) return 0;
        return score > 0 ? MATE_SCORE - score : -(MATE_SCORE + score);
    }

    Search() = delete;
};

} // namespace chesserazade

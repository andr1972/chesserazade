/// Search — minimax (negamax) with fixed depth and mate detection.
///
/// This is the 0.5 search: no pruning, no ordering, no hashing.
/// Every node re-generates its full legal-move list and walks every
/// child. The point is to be *obviously correct* — a reader can
/// trace `find_best` by hand on a two-move tree. Performance work
/// begins in 0.6 with alpha-beta, then 0.7 with transposition
/// tables.
///
/// Scoring conventions (all in centipawns, from the side-to-move's
/// perspective, negamax style):
///
///   * Positional scores: evaluator output, typically within
///     ±10000.
///   * Mate scores: `±(MATE_SCORE - ply)`. A mate delivered at
///     distance N plies from the root comes back as
///     `MATE_SCORE - N` if *we* deliver it, `-(MATE_SCORE - N)` if
///     the opponent does. This encoding lets the search *prefer
///     faster mates* — `MATE - 1` (mate in 1 ply) beats `MATE - 3`
///     (mate in 3 plies) which beats any non-mating score.
///   * Stalemate: returns 0 at the ply where it is detected.
///
/// `MATE_SCORE` is 32000, comfortably below `std::int16_t::max()`
/// so the engine could drop to 16-bit scores in a future
/// optimization without changing the API.
///
/// Reference: https://www.chessprogramming.org/Negamax
#pragma once

#include <chesserazade/move.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace chesserazade {

class Board;

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

    /// Number of nodes visited in the search tree. Includes
    /// interior nodes and leaves.
    std::uint64_t nodes = 0;

    /// Wall-clock time for the search.
    std::chrono::milliseconds elapsed{0};
};

class Search {
public:
    /// The "you are mated at this ply" sentinel. Fits in 16 bits.
    static constexpr int MATE_SCORE = 32000;

    /// Any positional score the evaluator produces must stay
    /// strictly below this. Used as the initial -∞ / +∞ alpha-
    /// beta window will use in 0.6.
    static constexpr int INF_SCORE = 32001;

    /// Maximum search depth this engine will accept. Shallow
    /// enough that the triangular PV table fits on the stack and
    /// the pure negamax completes in a human-bearable time on a
    /// mailbox board.
    static constexpr int MAX_DEPTH = 32;

    /// Find the best move in `board` at the given fixed depth.
    /// `board` is mutated during the search (make / unmake) and
    /// restored on return.
    [[nodiscard]] static SearchResult find_best(Board& board, int depth);

    /// True if `score` encodes a forced mate (won or lost).
    [[nodiscard]] static constexpr bool is_mate_score(int score) noexcept {
        // A "mate within MAX_DEPTH" score is safely separated from
        // any realistic positional score. The 1000-centipawn buffer
        // is comfortable over what the evaluator can produce.
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

/// PuzzleSolver — find forced mates from a given position.
///
/// Given a mate-in-N puzzle (a position where the side to move
/// has a forced sequence leading to checkmate within N full
/// chess moves), `solve_mate_in(board, N)` searches to a depth
/// that is guaranteed to *see* the mate if it exists, and
/// returns the search result — best move, PV, score, nodes,
/// elapsed.
///
/// The acceptance criterion for 0.8 (HANDOFF §9) is "solves a
/// curated set of mate-in-2 and mate-in-3 puzzles with correct
/// first move and full PV". This module is the entry point the
/// `solve --mate-in N` CLI uses, and it is the shape the REPL
/// will drive from 0.9+ when analyzing user-supplied puzzles.
///
/// Depth choice: a mate in N full moves is 2N-1 plies (white's
/// Nth move delivers mate, so the mated side has no legal reply
/// at ply 2N-1). Our negamax short-circuits to the evaluator at
/// `depth == 0` without a terminal check, so we need one extra
/// ply of headroom: `depth = 2N`. Iterative deepening stops
/// early on a found mate, so asking for depth 2N+1 is no waste.
///
/// Reference: https://www.chessprogramming.org/Puzzle
#pragma once

#include <chesserazade/search.hpp>

namespace chesserazade {

class Board;
class TranspositionTable;

class PuzzleSolver {
public:
    /// Search for a forced mate in exactly `moves` full chess
    /// moves (1 move = white + black = 2 plies, except the last
    /// white move which ends the game). Returns a full
    /// `SearchResult`; the caller should check
    /// `Search::is_mate_score(result.score)` and
    /// `Search::plies_to_mate(result.score)` to verify.
    ///
    /// `tt` is optional — if provided, it is reused across the
    /// iterative-deepening iterations (speeds up repeated
    /// solves on related positions). Pass `nullptr` for a
    /// one-shot solve.
    [[nodiscard]] static SearchResult solve_mate_in(Board& board,
                                                    int moves,
                                                    TranspositionTable* tt = nullptr);

    PuzzleSolver() = delete;
};

} // namespace chesserazade

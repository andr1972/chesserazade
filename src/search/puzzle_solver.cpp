// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include <chesserazade/puzzle_solver.hpp>

#include <chesserazade/board.hpp>
#include <chesserazade/search.hpp>
#include <chesserazade/transposition_table.hpp>

namespace chesserazade {

SearchResult PuzzleSolver::solve_mate_in(Board& board, int moves,
                                         TranspositionTable* tt) {
    // Clamp: mate-in-0 is nonsense ("you're already mated");
    // mate-in-large-N reaches the engine's MAX_DEPTH ceiling.
    int m = moves;
    if (m < 1) m = 1;

    // 2N-1 plies is the true tree depth of a mate-in-N line.
    // +1 ply gives negamax a plain leaf call (evaluator) beyond
    // the mating move so the terminal-move detection at the
    // final reply fires cleanly.
    int depth = 2 * m + 1;
    if (depth > Search::MAX_DEPTH) depth = Search::MAX_DEPTH;

    SearchLimits limits;
    limits.max_depth = depth;
    return Search::find_best(board, limits, tt);
}

} // namespace chesserazade

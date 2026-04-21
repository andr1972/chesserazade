/// Negamax search with mate scoring and a triangular PV table.
///
/// The entire algorithm fits in one recursive helper (`negamax`)
/// plus a driver (`find_best`). The classical chess-programming
/// approach, deliberately written without tricks:
///
/// ```
/// negamax(board, depth, ply):
///     if depth == 0:               return evaluate(board)
///     moves = legal_moves(board)
///     if moves is empty:
///         return is_in_check ? -MATE_SCORE + ply : 0     // mate / stalemate
///     best = -INF
///     for m in moves:
///         board.make_move(m)
///         score = -negamax(board, depth - 1, ply + 1)
///         board.unmake_move(m)
///         best = max(best, score)
///     return best
/// ```
///
/// The triangular PV table is the standard textbook structure for
/// extracting the best line without a transposition table:
///
/// ```
/// pv[ply][0..len[ply])  the PV starting at this ply
/// ```
///
/// When a node finds a new best move, it copies the child's PV
/// into its own, shifted by one slot and prepended with the
/// best move. See https://www.chessprogramming.org/Triangular_PV-Table

#include <chesserazade/search.hpp>

#include <chesserazade/board.hpp>
#include <chesserazade/evaluator.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/move_generator.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace chesserazade {

namespace {

using clk = std::chrono::steady_clock;

/// Triangular PV table. For each ply, `moves[ply]` stores the
/// principal variation starting at that ply (move at `ply`, move
/// at `ply+1`, …); `length[ply]` is how many of those slots are
/// filled. Everything lives on the stack — no allocations during
/// search.
constexpr std::size_t PV_SIZE = static_cast<std::size_t>(Search::MAX_DEPTH);

struct PvTable {
    std::array<std::array<Move, PV_SIZE>, PV_SIZE> moves{};
    std::array<std::size_t, PV_SIZE> length{};
};

/// Core negamax. `nodes` accumulates the visited-node count; the
/// PV table is updated incrementally. Returns the best score for
/// the side to move from `board`.
int negamax(Board& board, int depth, int ply, std::uint64_t& nodes,
            PvTable& pv) {
    ++nodes;

    const std::size_t p = static_cast<std::size_t>(ply);
    // Start fresh: this ply's PV is empty until a move is picked.
    pv.length[p] = 0;

    if (depth == 0) {
        return evaluate(board);
    }

    const MoveList legal = MoveGenerator::generate_legal(board);
    if (legal.empty()) {
        // Terminal: checkmate or stalemate.
        if (MoveGenerator::is_in_check(board, board.side_to_move())) {
            return -Search::MATE_SCORE + ply;
        }
        return 0; // stalemate
    }

    int best = -Search::INF_SCORE;
    for (const Move& m : legal) {
        board.make_move(m);
        const int score = -negamax(board, depth - 1, ply + 1, nodes, pv);
        board.unmake_move(m);

        if (score > best) {
            best = score;
            // Record the new best move at this ply and splice the
            // child's PV behind it.
            pv.moves[p][0] = m;
            const std::size_t child_len = pv.length[p + 1];
            for (std::size_t i = 0; i < child_len; ++i) {
                pv.moves[p][i + 1] = pv.moves[p + 1][i];
            }
            pv.length[p] = 1 + child_len;
        }
    }
    return best;
}

} // namespace

SearchResult Search::find_best(Board& board, int depth) {
    SearchResult result;

    if (depth < 0) depth = 0;
    if (depth > MAX_DEPTH) depth = MAX_DEPTH;

    const auto start = clk::now();

    // Handle the root terminal case explicitly: the caller may
    // invoke `find_best` on a position that is already checkmate
    // or stalemate. We don't want to return a default move in
    // that case; `best_move` stays default-constructed and
    // `score` reports the terminal value.
    const MoveList root_moves = MoveGenerator::generate_legal(board);
    if (root_moves.empty()) {
        if (MoveGenerator::is_in_check(board, board.side_to_move())) {
            result.score = -MATE_SCORE; // mated at ply 0
        } else {
            result.score = 0;           // stalemate
        }
        result.nodes = 1;
        result.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                clk::now() - start);
        return result;
    }

    PvTable pv;
    const int score = negamax(board, depth, 0, result.nodes, pv);
    result.score = score;

    const std::size_t pv_len = pv.length[0];
    if (pv_len > 0) {
        result.best_move = pv.moves[0][0];
        result.principal_variation.reserve(pv_len);
        for (std::size_t i = 0; i < pv_len; ++i) {
            result.principal_variation.push_back(pv.moves[0][i]);
        }
    } else {
        // `depth == 0`: nothing searched; fall back to the first
        // legal move so `best_move` is at least playable.
        result.best_move = *root_moves.begin();
    }

    result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clk::now() - start);
    return result;
}

} // namespace chesserazade

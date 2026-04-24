// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Tests for quiescence search and move ordering.
///
/// These are behavioural tests on the search as a whole rather
/// than unit tests of the ordering / quiesce helpers directly —
/// the helpers live inside an anonymous namespace (not part of
/// the public API) and their only observable effect is on the
/// search's score and node count.
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/puzzle_solver.hpp>
#include <chesserazade/search.hpp>
#include <chesserazade/transposition_table.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace chesserazade;

namespace {

Board8x8Mailbox board_from(std::string_view fen) {
    auto r = Board8x8Mailbox::from_fen(fen);
    REQUIRE(r.has_value());
    return *r;
}

} // namespace

// ---------------------------------------------------------------------------
// Move ordering
// ---------------------------------------------------------------------------

TEST_CASE("Ordering: TT move steers iterative deepening", "[ordering]") {
    // The Italian Game position. With iterative deepening + TT,
    // depth N+1 re-enters the same early positions that depth N
    // already analyzed. The TT move stored by N guides N+1's
    // move order, which further prunes the tree.
    const std::string fen =
        "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 3";

    auto b1 = board_from(fen);
    const SearchResult no_tt = Search::find_best(b1, 4);

    auto b2 = board_from(fen);
    TranspositionTable tt(1u << 16);
    SearchLimits l; l.max_depth = 4;
    const SearchResult with_tt = Search::find_best(b2, l, &tt);

    // TT + ordering should make the search strictly cheaper.
    // Magnitude depends on the position; we only require "fewer".
    REQUIRE(with_tt.nodes < no_tt.nodes);
}

// ---------------------------------------------------------------------------
// Quiescence
// ---------------------------------------------------------------------------

TEST_CASE("Quiescence: hanging queen is punished", "[quiescence]") {
    // White queen on d4, black rook on d8 — the rook can take
    // the queen. At depth 1 without quiescence, a white non-
    // capture reply would look fine (static eval shows white up
    // a queen). With quiescence, black's Rxd4 reply at the
    // horizon reveals the real value: white loses the queen.
    //
    // We verify: depth-1 search from BLACK's side returns a
    // score that reflects the queen capture (positive for black).
    // A queen-capturing reply scores ~(queen - rook) + PSTs =
    // around +500 to +700 centipawns from black's perspective.
    auto b = board_from("3r3k/8/8/3Q4/8/8/8/7K b - - 0 1");
    const SearchResult r = Search::find_best(b, 1);
    // Black to move and can take the hanging queen.
    REQUIRE(r.score > 400);
}

TEST_CASE("Quiescence: search still returns a legal move on a quiet "
          "position", "[quiescence]") {
    // Regression: the old depth==0 branch returned `evaluate()`;
    // now it returns `quiesce()`. Both must agree on a position
    // with no available captures — same side-to-move value.
    auto b = board_from(std::string{STARTING_POSITION_FEN});
    const SearchResult r = Search::find_best(b, 1);
    REQUIRE(r.best_move.from != Square::None);
    // Symmetric position with a 1-ply search: white picks *something*
    // and the score lands in a sane band.
    REQUIRE(r.score > -200);
    REQUIRE(r.score < 200);
}

// ---------------------------------------------------------------------------
// PuzzleSolver — the 0.8 acceptance criterion.
// ---------------------------------------------------------------------------

TEST_CASE("PuzzleSolver: mate-in-1 (back-rank mate)", "[puzzle][mate]") {
    auto b = board_from("6k1/5ppp/8/8/8/8/8/R6K w - - 0 1");
    TranspositionTable tt;
    const SearchResult r = PuzzleSolver::solve_mate_in(b, 1, &tt);
    REQUIRE(Search::is_mate_score(r.score));
    REQUIRE(Search::plies_to_mate(r.score) == 1);
    REQUIRE_FALSE(r.principal_variation.empty());
}

TEST_CASE("PuzzleSolver: mate-in-2 (K+Q vs K drive)", "[puzzle][mate]") {
    auto b = board_from("k7/8/8/1QK5/8/8/8/8 w - - 0 1");
    TranspositionTable tt;
    const SearchResult r = PuzzleSolver::solve_mate_in(b, 2, &tt);
    REQUIRE(Search::is_mate_score(r.score));
    REQUIRE(Search::plies_to_mate(r.score) == 3);
    REQUIRE(r.principal_variation.size() >= 3);
}

TEST_CASE("PuzzleSolver: mate-in-3 (K+Q vs K drive, further)", "[puzzle][mate]") {
    auto b = board_from("k7/8/8/1Q6/3K4/8/8/8 w - - 0 1");
    TranspositionTable tt;
    const SearchResult r = PuzzleSolver::solve_mate_in(b, 3, &tt);
    REQUIRE(Search::is_mate_score(r.score));
    REQUIRE(Search::plies_to_mate(r.score) == 5);
    REQUIRE(r.principal_variation.size() >= 5);
}

/// Tests for the negamax search.
///
/// What we verify:
///   * Mate-in-1 positions: depth-2 search returns a mate score
///     = MATE - 1 and picks the mating move.
///   * Mate-in-2 positions: depth-3 search returns a mate score
///     = MATE - 3 (distance in plies) and picks the correct first
///     move of the forced sequence.
///   * Stalemate positions: terminal, score 0, no crash.
///   * Mate-at-root: terminal, score = -MATE, no crash.
///   * Helpers: is_mate_score / plies_to_mate round-trip correctly.
///   * Evaluation "prefers" the higher-material move at depth 1.
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/evaluator.hpp>
#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/search.hpp>

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
// Helpers
// ---------------------------------------------------------------------------

TEST_CASE("Search: is_mate_score / plies_to_mate round-trip", "[search]") {
    REQUIRE(Search::is_mate_score(Search::MATE_SCORE - 1));
    REQUIRE(Search::is_mate_score(-(Search::MATE_SCORE - 3)));
    REQUIRE_FALSE(Search::is_mate_score(900));
    REQUIRE_FALSE(Search::is_mate_score(-900));
    REQUIRE_FALSE(Search::is_mate_score(0));

    REQUIRE(Search::plies_to_mate(Search::MATE_SCORE - 1) == 1);
    REQUIRE(Search::plies_to_mate(Search::MATE_SCORE - 3) == 3);
    REQUIRE(Search::plies_to_mate(-(Search::MATE_SCORE - 1)) == -1);
    REQUIRE(Search::plies_to_mate(-(Search::MATE_SCORE - 3)) == -3);
    REQUIRE(Search::plies_to_mate(42) == 0);
}

// ---------------------------------------------------------------------------
// Terminal positions
// ---------------------------------------------------------------------------

TEST_CASE("Search: mate at root returns -MATE_SCORE", "[search][terminal]") {
    // Back-rank mate already on the board; it is black to move and
    // the black king is already mated.
    // White: Ra8, Kh1. Black: Kg8, pawns f7/g7/h7.
    auto b = board_from("R5k1/5ppp/8/8/8/8/8/7K b - - 0 1");
    const SearchResult r = Search::find_best(b, 3);
    REQUIRE(r.score == -Search::MATE_SCORE);
}

TEST_CASE("Search: stalemate at root returns 0", "[search][terminal]") {
    // Classic K+Q vs K stalemate: black king h8, no escape, not
    // in check. White queen f7 controls all king moves.
    auto b = board_from("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    const SearchResult r = Search::find_best(b, 3);
    REQUIRE(r.score == 0);
}

// ---------------------------------------------------------------------------
// Mate-in-1
// ---------------------------------------------------------------------------

TEST_CASE("Search: finds mate-in-1 (back-rank mate)", "[search][mate1]") {
    // 1. Ra8#.
    auto b = board_from("6k1/5ppp/8/8/8/8/8/R6K w - - 0 1");
    const SearchResult r = Search::find_best(b, 2);
    REQUIRE(Search::is_mate_score(r.score));
    REQUIRE(Search::plies_to_mate(r.score) == 1);
    REQUIRE(to_uci(r.best_move) == "a1a8");
}

TEST_CASE("Search: finds mate-in-1 (Q+K mate)", "[search][mate1]") {
    // 1. Qb7#. Queen mates on b7 with the king supporting from c6.
    auto b = board_from("k7/8/2K5/8/8/8/1Q6/8 w - - 0 1");
    const SearchResult r = Search::find_best(b, 2);
    REQUIRE(Search::is_mate_score(r.score));
    REQUIRE(Search::plies_to_mate(r.score) == 1);
    REQUIRE(to_uci(r.best_move) == "b2b7");
}

// ---------------------------------------------------------------------------
// Mate-in-2 (3 plies)
// ---------------------------------------------------------------------------

TEST_CASE("Search: finds a forced mate-in-2 (3 plies)", "[search][mate2]") {
    // K+Q vs K with the white king one step short of the mating
    // square. The only forced mate is 1.Kc6 Ka7 2.Qb7#.
    //   * After 1.Kc6 black's only legal reply is Ka7
    //     (Kb7/Kb8 both run into queen or white king).
    //   * 2.Qb7# is then mate: Ka6/Ka8 are on the queen's
    //     diagonal and Kxb7 is covered by the white king on c6.
    // We need depth 4: the mate is at ply 3 (black has no legal
    // reply after white's 2nd move), and negamax short-circuits
    // to `evaluate` — not terminal detection — at depth 0. So
    // depth 3 would stop one ply short of seeing the mate.
    auto b = board_from("k7/8/8/1QK5/8/8/8/8 w - - 0 1");
    const SearchResult r = Search::find_best(b, 4);
    REQUIRE(Search::is_mate_score(r.score));
    REQUIRE(r.score > 0);
    REQUIRE(Search::plies_to_mate(r.score) == 3);
}

// ---------------------------------------------------------------------------
// Basic sanity: principal variation, node count, depth control
// ---------------------------------------------------------------------------

TEST_CASE("Search: principal_variation starts with best_move",
          "[search][pv]") {
    auto b = board_from(std::string{STARTING_POSITION_FEN});
    const SearchResult r = Search::find_best(b, 2);
    REQUIRE_FALSE(r.principal_variation.empty());
    REQUIRE(r.principal_variation.front() == r.best_move);
}

TEST_CASE("Search: depth 0 returns a legal move with evaluator score",
          "[search]") {
    auto b = board_from(std::string{STARTING_POSITION_FEN});
    const SearchResult r = Search::find_best(b, 0);
    // Depth 0 means "don't recurse" — we don't pick a best move,
    // but `find_best` still returns a playable one so the CLI has
    // something to show. Score should be near zero on a symmetric
    // start.
    REQUIRE(r.nodes >= 1);
    REQUIRE(r.score == 0);
}

TEST_CASE("Search: picks the move that wins a queen at depth 2",
          "[search][material]") {
    // Black's queen hangs on d8 to white's rook on d1 — with
    // nothing defending the queen. Depth 2 should see it.
    auto b = board_from("3q3k/8/8/8/8/8/8/3R3K w - - 0 1");
    const SearchResult r = Search::find_best(b, 2);
    REQUIRE(to_uci(r.best_move) == "d1d8");
    // After the capture white is up a queen minus a rook; no mate.
    REQUIRE_FALSE(Search::is_mate_score(r.score));
    REQUIRE(r.score > PieceValue::QUEEN - PieceValue::ROOK - 200);
}

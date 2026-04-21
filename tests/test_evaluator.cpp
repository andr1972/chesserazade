/// Tests for the Simplified Evaluation Function.
///
/// What we test:
///   * The starting position is perfectly symmetric, so the
///     score must be 0 regardless of whose turn it is.
///   * Material imbalances produce scores in the expected range.
///   * Mirror symmetry: swapping colors (and whose turn) negates
///     the score. This is the key invariant a negamax search
///     relies on.
///   * Piece values match the constants exposed in the header.
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/evaluator.hpp>
#include <chesserazade/fen.hpp>

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

TEST_CASE("evaluate: starting position is zero", "[eval]") {
    auto b = board_from(std::string{STARTING_POSITION_FEN});
    REQUIRE(evaluate(b) == 0);
}

TEST_CASE("evaluate: swapping turn in symmetric position keeps zero",
          "[eval]") {
    // The standard starting position with black to move is still
    // perfectly symmetric — the score should still be zero.
    auto b = board_from(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");
    REQUIRE(evaluate(b) == 0);
}

TEST_CASE("evaluate: an extra queen is worth roughly a queen", "[eval]") {
    // White starts with all pieces; black is missing its queen.
    // From white's perspective, the score should be close to
    // QUEEN_VALUE — give or take a few centipawns for the
    // piece-square-table delta of the missing queen.
    auto b = board_from(
        "rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const int s = evaluate(b);
    REQUIRE(s > PieceValue::QUEEN - 100);
    REQUIRE(s < PieceValue::QUEEN + 100);
}

TEST_CASE("evaluate: side-to-move perspective flips sign", "[eval]") {
    // Same material imbalance as above, but black to move. The
    // evaluator returns from *side-to-move*'s perspective, so the
    // sign must flip.
    auto b = board_from(
        "rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");
    const int s = evaluate(b);
    REQUIRE(s < -(PieceValue::QUEEN - 100));
    REQUIRE(s > -(PieceValue::QUEEN + 100));
}

TEST_CASE("evaluate: mirror invariance", "[eval]") {
    // Mirroring a position (swap colors + swap ranks + flip side
    // to move) must produce the same score. This is the negamax
    // foundational identity.
    auto w = board_from("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1");
    auto b = board_from("4k3/4p3/8/8/8/8/8/4K3 b - - 0 1");
    REQUIRE(evaluate(w) == evaluate(b));
}

TEST_CASE("evaluate: knight on d4 beats knight on a1", "[eval]") {
    // Central knight scores higher than a knight in the corner,
    // per the classical PST. With identical material otherwise
    // the difference must be strictly positive and reflect the
    // PST gap (15 - (-50) = 65 centipawns in the Simplified table).
    auto central = board_from("4k3/8/8/8/3N4/8/8/4K3 w - - 0 1");
    auto corner  = board_from("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    const int gap = evaluate(central) - evaluate(corner);
    REQUIRE(gap == 70); // 20 on d4 vs -50 on a1
}

TEST_CASE("evaluate: pawn on rank 7 worth more than rank 2", "[eval]") {
    auto advanced = board_from("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    auto home     = board_from("4k3/8/8/8/8/8/P7/4K3 w - - 0 1");
    REQUIRE(evaluate(advanced) > evaluate(home));
}

TEST_CASE("piece_value: constants match the enum", "[eval]") {
    REQUIRE(piece_value(PieceType::Pawn)   == 100);
    REQUIRE(piece_value(PieceType::Knight) == 320);
    REQUIRE(piece_value(PieceType::Bishop) == 330);
    REQUIRE(piece_value(PieceType::Rook)   == 500);
    REQUIRE(piece_value(PieceType::Queen)  == 900);
    REQUIRE(piece_value(PieceType::None)   == 0);
}

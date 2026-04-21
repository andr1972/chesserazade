/// Perft correctness on the BoardBitboard implementation.
///
/// The existing `MoveGenerator` uses the abstract `Board&`
/// interface, so it also works on `BoardBitboard`. These tests
/// verify that make_move / unmake_move on the bitboard
/// implementation produce the same leaf counts as the mailbox
/// on every standard perft position.
///
/// We cap the depth at 4 here — depth 5 through the abstract
/// `piece_at` is slow on a bitboard board (virtual dispatch +
/// linear piece-type walk), and a bitboard-native move
/// generator that makes depth 5 fast lands in 1.1.4. Depth 4
/// is enough for correctness: the node counts at depth 4 are
/// distinct across positions and already in the hundreds of
/// thousands, so any make/unmake bug would show up.
#include "board/board_bitboard.hpp"

#include <chesserazade/move_generator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace chesserazade;

namespace {

std::uint64_t perft(Board& b, int depth) {
    if (depth == 0) return 1;
    const MoveList ml = MoveGenerator::generate_legal(b);
    if (depth == 1) return static_cast<std::uint64_t>(ml.count);
    std::uint64_t n = 0;
    for (const Move& m : ml) {
        b.make_move(m);
        n += perft(b, depth - 1);
        b.unmake_move(m);
    }
    return n;
}

BoardBitboard bb_from(std::string_view fen) {
    auto r = BoardBitboard::from_fen(fen);
    REQUIRE(r.has_value());
    return *r;
}

} // namespace

TEST_CASE("perft bitboard: initial position", "[perft][bitboard]") {
    auto b = bb_from("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    REQUIRE(perft(b, 1) == 20ULL);
    REQUIRE(perft(b, 2) == 400ULL);
    REQUIRE(perft(b, 3) == 8902ULL);
    REQUIRE(perft(b, 4) == 197281ULL);
}

TEST_CASE("perft bitboard: Kiwipete", "[perft][bitboard]") {
    auto b = bb_from(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    REQUIRE(perft(b, 1) == 48ULL);
    REQUIRE(perft(b, 2) == 2039ULL);
    REQUIRE(perft(b, 3) == 97862ULL);
    REQUIRE(perft(b, 4) == 4085603ULL);
}

TEST_CASE("perft bitboard: position 3", "[perft][bitboard]") {
    auto b = bb_from("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    REQUIRE(perft(b, 1) == 14ULL);
    REQUIRE(perft(b, 2) == 191ULL);
    REQUIRE(perft(b, 3) == 2812ULL);
    REQUIRE(perft(b, 4) == 43238ULL);
}

TEST_CASE("perft bitboard: position 4", "[perft][bitboard]") {
    auto b = bb_from(
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");
    REQUIRE(perft(b, 1) == 6ULL);
    REQUIRE(perft(b, 2) == 264ULL);
    REQUIRE(perft(b, 3) == 9467ULL);
    REQUIRE(perft(b, 4) == 422333ULL);
}

TEST_CASE("perft bitboard: position 5", "[perft][bitboard]") {
    auto b = bb_from(
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8");
    REQUIRE(perft(b, 1) == 44ULL);
    REQUIRE(perft(b, 2) == 1486ULL);
    REQUIRE(perft(b, 3) == 62379ULL);
    REQUIRE(perft(b, 4) == 2103487ULL);
}

TEST_CASE("perft bitboard: position 6", "[perft][bitboard]") {
    auto b = bb_from(
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10");
    REQUIRE(perft(b, 1) == 46ULL);
    REQUIRE(perft(b, 2) == 2079ULL);
    REQUIRE(perft(b, 3) == 89890ULL);
    REQUIRE(perft(b, 4) == 3894594ULL);
}

/// Perft correctness tests.
///
/// "Perft" (performance test, move path enumeration) counts the number
/// of leaf nodes at a given depth in the game tree starting from a
/// known position.  Matching the reference counts is the standard way
/// to validate a move generator: any bug (illegal move generated,
/// legal move missing, wrong make/unmake) will surface as a count
/// mismatch at some depth.
///
/// Reference counts come from the Chess Programming Wiki:
///   https://www.chessprogramming.org/Perft_Results
///
/// Test policy (HANDOFF §7):
///   * Depths 1..5 run in CI for all six standard positions.
///   * Depth 6 for the initial position is verified by the CLI
///     `perft` command and `tools/perft_bench.cpp`; it is NOT run
///     in automated CI because it takes minutes on a mailbox engine.
///   * Tests use ONLY depth/node limits — no wall-clock timeouts.
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/move_generator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace chesserazade;

namespace {

/// Recursive perft: count leaf nodes at `depth`. A leaf is a node
/// at depth 0 — we count positions, not moves.
std::uint64_t perft(Board& b, int depth) {
    if (depth == 0) {
        return 1;
    }
    const MoveList moves = MoveGenerator::generate_legal(b);
    if (depth == 1) {
        return static_cast<std::uint64_t>(moves.count);
    }
    std::uint64_t nodes = 0;
    for (const Move& m : moves) {
        b.make_move(m);
        nodes += perft(b, depth - 1);
        b.unmake_move(m);
    }
    return nodes;
}

Board8x8Mailbox board_from(std::string_view fen) {
    auto result = Board8x8Mailbox::from_fen(fen);
    REQUIRE(result.has_value());
    return *result;
}

} // namespace

// ---------------------------------------------------------------------------
// Position 1 — Initial position
// Ref: https://www.chessprogramming.org/Perft_Results#Initial_Position
// ---------------------------------------------------------------------------
TEST_CASE("perft initial position", "[perft][pos1]") {
    auto b = board_from("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    REQUIRE(perft(b, 1) == 20ULL);
    REQUIRE(perft(b, 2) == 400ULL);
    REQUIRE(perft(b, 3) == 8902ULL);
    REQUIRE(perft(b, 4) == 197281ULL);
    REQUIRE(perft(b, 5) == 4865609ULL);
    // Depth 6 (119 060 324) is verified via the CLI, not in CI.
}

// ---------------------------------------------------------------------------
// Position 2 — Kiwipete (many special moves)
// Ref: https://www.chessprogramming.org/Perft_Results#Position_2
// ---------------------------------------------------------------------------
TEST_CASE("perft Kiwipete", "[perft][pos2]") {
    auto b = board_from(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    REQUIRE(perft(b, 1) == 48ULL);
    REQUIRE(perft(b, 2) == 2039ULL);
    REQUIRE(perft(b, 3) == 97862ULL);
    REQUIRE(perft(b, 4) == 4085603ULL);
    REQUIRE(perft(b, 5) == 193690690ULL);
}

// ---------------------------------------------------------------------------
// Position 3 — Endgame with en-passant and promotion
// Ref: https://www.chessprogramming.org/Perft_Results#Position_3
// ---------------------------------------------------------------------------
TEST_CASE("perft position 3", "[perft][pos3]") {
    auto b = board_from("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    REQUIRE(perft(b, 1) == 14ULL);
    REQUIRE(perft(b, 2) == 191ULL);
    REQUIRE(perft(b, 3) == 2812ULL);
    REQUIRE(perft(b, 4) == 43238ULL);
    REQUIRE(perft(b, 5) == 674624ULL);
}

// ---------------------------------------------------------------------------
// Position 4 — Mirror of Position 3, black to move
// Ref: https://www.chessprogramming.org/Perft_Results#Position_4_and_5
// ---------------------------------------------------------------------------
TEST_CASE("perft position 4", "[perft][pos4]") {
    auto b = board_from(
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");
    REQUIRE(perft(b, 1) == 6ULL);
    REQUIRE(perft(b, 2) == 264ULL);
    REQUIRE(perft(b, 3) == 9467ULL);
    REQUIRE(perft(b, 4) == 422333ULL);
    REQUIRE(perft(b, 5) == 15833292ULL);
}

// ---------------------------------------------------------------------------
// Position 5 — Complex middle game
// Ref: https://www.chessprogramming.org/Perft_Results#Position_5
// ---------------------------------------------------------------------------
TEST_CASE("perft position 5", "[perft][pos5]") {
    auto b = board_from(
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8");
    REQUIRE(perft(b, 1) == 44ULL);
    REQUIRE(perft(b, 2) == 1486ULL);
    REQUIRE(perft(b, 3) == 62379ULL);
    REQUIRE(perft(b, 4) == 2103487ULL);
    REQUIRE(perft(b, 5) == 89941194ULL);
}

// ---------------------------------------------------------------------------
// Position 6 — Symmetric position
// Ref: https://www.chessprogramming.org/Perft_Results#Position_6
// ---------------------------------------------------------------------------
TEST_CASE("perft position 6", "[perft][pos6]") {
    auto b = board_from(
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10");
    REQUIRE(perft(b, 1) == 46ULL);
    REQUIRE(perft(b, 2) == 2079ULL);
    REQUIRE(perft(b, 3) == 89890ULL);
    REQUIRE(perft(b, 4) == 3894594ULL);
    REQUIRE(perft(b, 5) == 164075551ULL);
}

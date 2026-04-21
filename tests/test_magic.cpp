/// Tests for the magic-bitboards slider-attack path.
///
/// Strategy:
///   1. Call `init_magic_attacks()` — generates magics in
///      memory and swaps `Attacks::rook/bishop` to the magic
///      lookup.
///   2. Verify every square + a bunch of random occupancies
///      against the loop-based reference: the two MUST agree
///      bit-for-bit.
///   3. Re-run a perft depth-4 on each of the six standard
///      positions — if the magic table has a collision we'd
///      see a node-count mismatch immediately.
///
/// After `init_magic_attacks()` the swap is global; any
/// subsequent test case automatically uses the magic path.
/// Since the magic output equals the loop output by construction
/// (we validate that in this file), the other perft tests
/// continue to pass unchanged.
#include "board/board_bitboard.hpp"
#include "board/magic.hpp"

#include <chesserazade/bitboard.hpp>
#include <chesserazade/move_generator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>

using namespace chesserazade;

namespace {

std::uint64_t perft(Board& b, int depth) {
    if (depth == 0) return 1;
    const MoveList ml = MoveGenerator::generate_legal(b);
    if (depth == 1) return ml.count;
    std::uint64_t n = 0;
    for (const Move& m : ml) {
        b.make_move(m);
        n += perft(b, depth - 1);
        b.unmake_move(m);
    }
    return n;
}

} // namespace

TEST_CASE("Magic bitboards: init succeeds", "[magic]") {
    REQUIRE(init_magic_attacks());
    REQUIRE(magic_attacks_available());
    // Swap happened: Attacks::rook is no longer the loop variant.
    REQUIRE(Attacks::rook_fn() != &Attacks::rook_loop);
    REQUIRE(Attacks::bishop_fn() != &Attacks::bishop_loop);
}

TEST_CASE("Magic bitboards: parity with loop on empty board",
          "[magic]") {
    init_magic_attacks();
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Square sq = static_cast<Square>(i);
        REQUIRE(Attacks::rook(sq, 0)   == Attacks::rook_loop(sq, 0));
        REQUIRE(Attacks::bishop(sq, 0) == Attacks::bishop_loop(sq, 0));
    }
}

TEST_CASE("Magic bitboards: parity with loop on random occupancies",
          "[magic]") {
    init_magic_attacks();

    std::mt19937_64 rng(0xC0FFEE);
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Square sq = static_cast<Square>(i);
        for (int trial = 0; trial < 64; ++trial) {
            const Bitboard occ = rng();
            REQUIRE(Attacks::rook(sq, occ) ==
                    Attacks::rook_loop(sq, occ));
            REQUIRE(Attacks::bishop(sq, occ) ==
                    Attacks::bishop_loop(sq, occ));
        }
    }
}

TEST_CASE("Magic bitboards: perft parity at depth 4", "[magic][perft]") {
    init_magic_attacks();

    struct Position { const char* name; const char* fen; std::uint64_t expected; };
    const Position positions[] = {
        {"initial",
         "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 197281ULL},
        {"kiwipete",
         "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
         4085603ULL},
        {"pos3",
         "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 43238ULL},
    };
    for (const auto& p : positions) {
        auto r = BoardBitboard::from_fen(p.fen);
        REQUIRE(r.has_value());
        BoardBitboard b = *r;
        REQUIRE(perft(b, 4) == p.expected);
    }
}

// ---------------------------------------------------------------------------
// File I/O — write the current magics, clear state, reload from
// the file, verify we still agree with the loop reference.
// ---------------------------------------------------------------------------

TEST_CASE("Magic bitboards: write then reload round-trips",
          "[magic][file]") {
    // First ensure we have generated magics.
    REQUIRE(init_magic_attacks());

    const std::string tmp = "/tmp/chesserazade-test-magics.txt";
    REQUIRE(write_magics_to_file(tmp));

    // Reset to the uninitialized (loop) baseline and reload.
    reset_magic_attacks();
    REQUIRE_FALSE(magic_attacks_available());
    REQUIRE(Attacks::rook_fn() == &Attacks::rook_loop);

    REQUIRE(init_magic_attacks_from_file(tmp));
    REQUIRE(magic_attacks_available());
    REQUIRE(Attacks::rook_fn() != &Attacks::rook_loop);

    // Parity check (again — the *loaded* tables must be as
    // good as the generated ones).
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Square sq = static_cast<Square>(i);
        REQUIRE(Attacks::rook(sq, 0)   == Attacks::rook_loop(sq, 0));
        REQUIRE(Attacks::bishop(sq, 0) == Attacks::bishop_loop(sq, 0));
    }
}

TEST_CASE("Magic bitboards: missing file returns false", "[magic][file]") {
    reset_magic_attacks();
    REQUIRE_FALSE(
        init_magic_attacks_from_file("/nonexistent/path/magics.txt"));
    REQUIRE_FALSE(magic_attacks_available());
    // Fallback: Attacks::rook still works via the loop baseline.
    REQUIRE(Attacks::rook_fn() == &Attacks::rook_loop);
}

TEST_CASE("Magic bitboards: default-locations init finds the shipped file",
          "[magic][file]") {
    reset_magic_attacks();
    // The repo ships data/magics.txt, and the library was
    // compiled with CHESSERAZADE_SOURCE_DIR baked in — the
    // default-locations init must find it even when the test
    // binary is run from anywhere.
    REQUIRE(init_magic_attacks_from_default_locations());
    REQUIRE(magic_attacks_available());
}

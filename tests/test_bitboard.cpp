// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Tests for bitboard primitives and non-slider attack tables.
///
/// Two kinds of assertion:
///   1. Shape — the bit operations round-trip, popcount agrees
///      with a hand-counted mask, file/rank constants alias to
///      the right 8-square sets.
///   2. Attack tables — a knight on d4 attacks 8 squares, on
///      a1 attacks 2. A king at e1 has 5 neighbours. A white
///      pawn on e4 attacks d5 and f5; a black pawn on e5
///      attacks d4 and f4. Pawns on their promotion rank
///      return the correct empty set.
#include <chesserazade/bitboard.hpp>
#include <chesserazade/types.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace chesserazade;

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

TEST_CASE("Bitboard: bb_of produces exactly one bit", "[bitboard]") {
    REQUIRE(popcount(bb_of(Square::A1)) == 1);
    REQUIRE(popcount(bb_of(Square::H8)) == 1);
    REQUIRE(bb_of(Square::A1) == 1ULL);
    REQUIRE(bb_of(Square::H8) == (1ULL << 63));
}

TEST_CASE("Bitboard: contains / set / clear round-trip", "[bitboard]") {
    Bitboard b = 0;
    REQUIRE_FALSE(contains(b, Square::E4));
    b = set(b, Square::E4);
    REQUIRE(contains(b, Square::E4));
    REQUIRE(popcount(b) == 1);
    b = clear(b, Square::E4);
    REQUIRE_FALSE(contains(b, Square::E4));
    REQUIRE(b == 0);
}

TEST_CASE("Bitboard: popcount of full and empty boards", "[bitboard]") {
    REQUIRE(popcount(ALL_SQUARES) == 64);
    REQUIRE(popcount(NO_SQUARES) == 0);
}

TEST_CASE("Bitboard: lsb / msb / pop_lsb", "[bitboard]") {
    Bitboard b = bb_of(Square::B2) | bb_of(Square::F7);
    REQUIRE(lsb(b) == Square::B2);
    REQUIRE(msb(b) == Square::F7);

    Square s1 = pop_lsb(b);
    REQUIRE(s1 == Square::B2);
    REQUIRE(popcount(b) == 1);
    Square s2 = pop_lsb(b);
    REQUIRE(s2 == Square::F7);
    REQUIRE(b == 0);
}

TEST_CASE("Bitboard: file and rank masks have 8 bits each", "[bitboard]") {
    REQUIRE(popcount(FILE_A) == 8);
    REQUIRE(popcount(FILE_H) == 8);
    REQUIRE(popcount(RANK_1) == 8);
    REQUIRE(popcount(RANK_8) == 8);

    // FILE_A contains a1..a8 and nothing else.
    REQUIRE(contains(FILE_A, Square::A1));
    REQUIRE(contains(FILE_A, Square::A8));
    REQUIRE_FALSE(contains(FILE_A, Square::B1));

    // RANK_1 is a1..h1.
    REQUIRE(contains(RANK_1, Square::A1));
    REQUIRE(contains(RANK_1, Square::H1));
    REQUIRE_FALSE(contains(RANK_1, Square::A2));
}

// ---------------------------------------------------------------------------
// Attack tables
// ---------------------------------------------------------------------------

TEST_CASE("Attacks::king: corner attacks 3 squares", "[bitboard][attacks]") {
    const Bitboard a = Attacks::king(Square::A1);
    REQUIRE(popcount(a) == 3);
    REQUIRE(contains(a, Square::A2));
    REQUIRE(contains(a, Square::B1));
    REQUIRE(contains(a, Square::B2));
}

TEST_CASE("Attacks::king: center attacks 8 squares", "[bitboard][attacks]") {
    const Bitboard a = Attacks::king(Square::E4);
    REQUIRE(popcount(a) == 8);
    REQUIRE(contains(a, Square::D3));
    REQUIRE(contains(a, Square::D4));
    REQUIRE(contains(a, Square::D5));
    REQUIRE(contains(a, Square::E3));
    REQUIRE(contains(a, Square::E5));
    REQUIRE(contains(a, Square::F3));
    REQUIRE(contains(a, Square::F4));
    REQUIRE(contains(a, Square::F5));
}

TEST_CASE("Attacks::knight: corner attacks 2 squares", "[bitboard][attacks]") {
    const Bitboard a = Attacks::knight(Square::A1);
    REQUIRE(popcount(a) == 2);
    REQUIRE(contains(a, Square::B3));
    REQUIRE(contains(a, Square::C2));
}

TEST_CASE("Attacks::knight: center attacks 8 squares", "[bitboard][attacks]") {
    const Bitboard a = Attacks::knight(Square::D4);
    REQUIRE(popcount(a) == 8);
    REQUIRE(contains(a, Square::B3));
    REQUIRE(contains(a, Square::B5));
    REQUIRE(contains(a, Square::C2));
    REQUIRE(contains(a, Square::C6));
    REQUIRE(contains(a, Square::E2));
    REQUIRE(contains(a, Square::E6));
    REQUIRE(contains(a, Square::F3));
    REQUIRE(contains(a, Square::F5));
}

TEST_CASE("Attacks::pawn: white on e4 attacks d5 and f5",
          "[bitboard][attacks]") {
    const Bitboard a = Attacks::pawn(Color::White, Square::E4);
    REQUIRE(popcount(a) == 2);
    REQUIRE(contains(a, Square::D5));
    REQUIRE(contains(a, Square::F5));
}

TEST_CASE("Attacks::pawn: black on e5 attacks d4 and f4",
          "[bitboard][attacks]") {
    const Bitboard a = Attacks::pawn(Color::Black, Square::E5);
    REQUIRE(popcount(a) == 2);
    REQUIRE(contains(a, Square::D4));
    REQUIRE(contains(a, Square::F4));
}

TEST_CASE("Attacks::pawn: file-a pawn only attacks one diagonal",
          "[bitboard][attacks]") {
    const Bitboard w = Attacks::pawn(Color::White, Square::A2);
    REQUIRE(popcount(w) == 1);
    REQUIRE(contains(w, Square::B3));

    const Bitboard b = Attacks::pawn(Color::Black, Square::H7);
    REQUIRE(popcount(b) == 1);
    REQUIRE(contains(b, Square::G6));
}

TEST_CASE("Attacks::pawn: rank-1/8 pawn has empty attacks",
          "[bitboard][attacks]") {
    // Malformed but well-defined: a white pawn on rank 8 has
    // no forward diagonals to attack.
    REQUIRE(Attacks::pawn(Color::White, Square::A8) == 0);
    REQUIRE(Attacks::pawn(Color::Black, Square::A1) == 0);
}

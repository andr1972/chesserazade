// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include <chesserazade/move.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace chesserazade;

TEST_CASE("to_uci renders quiet moves", "[move][uci]") {
    REQUIRE(to_uci(Move{Square::E2, Square::E4, PieceType::None}) == "e2e4");
    REQUIRE(to_uci(Move{Square::G1, Square::F3, PieceType::None}) == "g1f3");
    REQUIRE(to_uci(Move{Square::A1, Square::H8, PieceType::None}) == "a1h8");
}

TEST_CASE("to_uci renders promotions with lowercase suffix", "[move][uci][promotion]") {
    REQUIRE(to_uci(Move{Square::E7, Square::E8, PieceType::Queen})  == "e7e8q");
    REQUIRE(to_uci(Move{Square::E7, Square::E8, PieceType::Rook})   == "e7e8r");
    REQUIRE(to_uci(Move{Square::E7, Square::E8, PieceType::Bishop}) == "e7e8b");
    REQUIRE(to_uci(Move{Square::E7, Square::E8, PieceType::Knight}) == "e7e8n");
    REQUIRE(to_uci(Move{Square::A2, Square::A1, PieceType::Queen})  == "a2a1q");
}

TEST_CASE("to_uci handles castling as king-move notation", "[move][uci][castling]") {
    // Castling is encoded by the caller as the king's from/to squares.
    REQUIRE(to_uci(Move{Square::E1, Square::G1, PieceType::None}) == "e1g1");
    REQUIRE(to_uci(Move{Square::E1, Square::C1, PieceType::None}) == "e1c1");
    REQUIRE(to_uci(Move{Square::E8, Square::G8, PieceType::None}) == "e8g8");
    REQUIRE(to_uci(Move{Square::E8, Square::C8, PieceType::None}) == "e8c8");
}

TEST_CASE("Move equality is structural", "[move]") {
    const Move a{Square::E2, Square::E4, PieceType::None};
    const Move b{Square::E2, Square::E4, PieceType::None};
    const Move c{Square::E2, Square::E5, PieceType::None};
    const Move d{Square::E7, Square::E8, PieceType::Queen};
    const Move e{Square::E7, Square::E8, PieceType::Rook};
    REQUIRE(a == b);
    REQUIRE_FALSE(a == c);
    REQUIRE_FALSE(d == e);
}

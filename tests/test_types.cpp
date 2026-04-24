// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include <chesserazade/types.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace chesserazade;

TEST_CASE("Color::opposite round-trips", "[types][color]") {
    REQUIRE(opposite(Color::White) == Color::Black);
    REQUIRE(opposite(Color::Black) == Color::White);
    REQUIRE(opposite(opposite(Color::White)) == Color::White);
}

TEST_CASE("Piece::none is empty", "[types][piece]") {
    const Piece empty = Piece::none();
    REQUIRE(empty.is_none());
    REQUIRE(empty.type == PieceType::None);
}

TEST_CASE("Piece equality is structural", "[types][piece]") {
    const Piece wp{PieceType::Pawn, Color::White};
    const Piece wp2{PieceType::Pawn, Color::White};
    const Piece bp{PieceType::Pawn, Color::Black};
    REQUIRE(wp == wp2);
    REQUIRE_FALSE(wp == bp);
}

TEST_CASE("piece_to_fen_char covers all 12 pieces", "[types][piece][fen]") {
    REQUIRE(piece_to_fen_char({PieceType::Pawn,   Color::White}) == 'P');
    REQUIRE(piece_to_fen_char({PieceType::Knight, Color::White}) == 'N');
    REQUIRE(piece_to_fen_char({PieceType::Bishop, Color::White}) == 'B');
    REQUIRE(piece_to_fen_char({PieceType::Rook,   Color::White}) == 'R');
    REQUIRE(piece_to_fen_char({PieceType::Queen,  Color::White}) == 'Q');
    REQUIRE(piece_to_fen_char({PieceType::King,   Color::White}) == 'K');
    REQUIRE(piece_to_fen_char({PieceType::Pawn,   Color::Black}) == 'p');
    REQUIRE(piece_to_fen_char({PieceType::Knight, Color::Black}) == 'n');
    REQUIRE(piece_to_fen_char({PieceType::Bishop, Color::Black}) == 'b');
    REQUIRE(piece_to_fen_char({PieceType::Rook,   Color::Black}) == 'r');
    REQUIRE(piece_to_fen_char({PieceType::Queen,  Color::Black}) == 'q');
    REQUIRE(piece_to_fen_char({PieceType::King,   Color::Black}) == 'k');
}

TEST_CASE("piece_from_fen_char round-trips", "[types][piece][fen]") {
    constexpr std::string_view all = "PNBRQKpnbrqk";
    for (char c : all) {
        const auto p = piece_from_fen_char(c);
        REQUIRE(p.has_value());
        REQUIRE(piece_to_fen_char(*p) == c);
    }
}

TEST_CASE("piece_from_fen_char rejects invalid characters", "[types][piece][fen]") {
    for (char c : std::string_view{".xX12345678/- "}) {
        REQUIRE_FALSE(piece_from_fen_char(c).has_value());
    }
}

TEST_CASE("Square LERF mapping is consistent", "[types][square]") {
    REQUIRE(to_index(Square::A1) == 0);
    REQUIRE(to_index(Square::H1) == 7);
    REQUIRE(to_index(Square::A8) == 56);
    REQUIRE(to_index(Square::H8) == 63);
    REQUIRE(to_index(Square::E4) == 28);
    REQUIRE(is_valid(Square::A1));
    REQUIRE(is_valid(Square::H8));
    REQUIRE_FALSE(is_valid(Square::None));
}

TEST_CASE("make_square / file_of / rank_of round-trip", "[types][square]") {
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const auto sq = static_cast<Square>(i);
        const auto rebuilt = make_square(file_of(sq), rank_of(sq));
        REQUIRE(rebuilt == sq);
    }
}

TEST_CASE("Square algebraic conversion round-trips", "[types][square][algebraic]") {
    REQUIRE(to_algebraic(Square::A1) == "a1");
    REQUIRE(to_algebraic(Square::E4) == "e4");
    REQUIRE(to_algebraic(Square::H8) == "h8");

    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const auto sq = static_cast<Square>(i);
        const auto text = to_algebraic(sq);
        const auto parsed = square_from_algebraic(text);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == sq);
    }
}

TEST_CASE("square_from_algebraic rejects bad input", "[types][square][algebraic]") {
    REQUIRE_FALSE(square_from_algebraic("").has_value());
    REQUIRE_FALSE(square_from_algebraic("a").has_value());
    REQUIRE_FALSE(square_from_algebraic("a12").has_value());
    REQUIRE_FALSE(square_from_algebraic("i1").has_value());
    REQUIRE_FALSE(square_from_algebraic("a9").has_value());
    REQUIRE_FALSE(square_from_algebraic("A1").has_value()); // uppercase file not accepted
    REQUIRE_FALSE(square_from_algebraic("a0").has_value());
}

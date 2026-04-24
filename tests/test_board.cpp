// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/board.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace chesserazade;

TEST_CASE("default Board8x8Mailbox is empty, white to move", "[board]") {
    Board8x8Mailbox b;
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        REQUIRE(b.piece_at(static_cast<Square>(i)).is_none());
    }
    REQUIRE(b.side_to_move() == Color::White);
    REQUIRE_FALSE(b.castling_rights().any());
    REQUIRE(b.en_passant_square() == Square::None);
    REQUIRE(b.halfmove_clock() == 0);
    REQUIRE(b.fullmove_number() == 1);
}

TEST_CASE("set_piece_at / piece_at round-trip", "[board]") {
    Board8x8Mailbox b;
    const Piece white_king{PieceType::King, Color::White};
    const Piece black_queen{PieceType::Queen, Color::Black};

    b.set_piece_at(Square::E1, white_king);
    b.set_piece_at(Square::D8, black_queen);

    REQUIRE(b.piece_at(Square::E1) == white_king);
    REQUIRE(b.piece_at(Square::D8) == black_queen);
    REQUIRE(b.piece_at(Square::A1).is_none());

    // Overwriting and clearing via Piece::none().
    b.set_piece_at(Square::E1, Piece::none());
    REQUIRE(b.piece_at(Square::E1).is_none());
}

TEST_CASE("clear resets every field", "[board]") {
    Board8x8Mailbox b;
    b.set_piece_at(Square::A1, Piece{PieceType::Rook, Color::White});
    b.set_side_to_move(Color::Black);
    b.set_castling_rights(CastlingRights{true, true, true, true});
    b.set_en_passant_square(Square::E3);
    b.set_halfmove_clock(42);
    b.set_fullmove_number(17);

    b.clear();

    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        REQUIRE(b.piece_at(static_cast<Square>(i)).is_none());
    }
    REQUIRE(b.side_to_move() == Color::White);
    REQUIRE_FALSE(b.castling_rights().any());
    REQUIRE(b.en_passant_square() == Square::None);
    REQUIRE(b.halfmove_clock() == 0);
    REQUIRE(b.fullmove_number() == 1);
}

TEST_CASE("setters feed through to the abstract interface", "[board][interface]") {
    Board8x8Mailbox b;
    b.set_side_to_move(Color::Black);
    b.set_castling_rights(CastlingRights{true, false, false, true});
    b.set_en_passant_square(Square::D6);
    b.set_halfmove_clock(5);
    b.set_fullmove_number(23);

    const Board& view = b;
    REQUIRE(view.side_to_move() == Color::Black);
    const auto cr = view.castling_rights();
    REQUIRE(cr.white_king_side);
    REQUIRE_FALSE(cr.white_queen_side);
    REQUIRE_FALSE(cr.black_king_side);
    REQUIRE(cr.black_queen_side);
    REQUIRE(view.en_passant_square() == Square::D6);
    REQUIRE(view.halfmove_clock() == 5);
    REQUIRE(view.fullmove_number() == 23);
}

TEST_CASE("Board8x8Mailbox equality is structural", "[board]") {
    Board8x8Mailbox a;
    Board8x8Mailbox b;
    REQUIRE(a == b);

    a.set_piece_at(Square::E4, Piece{PieceType::Pawn, Color::White});
    REQUIRE_FALSE(a == b);

    b.set_piece_at(Square::E4, Piece{PieceType::Pawn, Color::White});
    REQUIRE(a == b);

    a.set_halfmove_clock(1);
    REQUIRE_FALSE(a == b);
}

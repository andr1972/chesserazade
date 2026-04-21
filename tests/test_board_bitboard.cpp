/// Tests for the bitboard Board implementation.
///
/// The 1.1.2 skeleton does not yet support make_move — those
/// tests live in 1.1.3's test_perft_bitboard.cpp. What we
/// verify here:
///   * from_fen populates the twelve piece bitboards and the
///     occupancy caches consistently;
///   * piece_at agrees with the mailbox board on every square
///     of the standard starting position and of Kiwipete;
///   * zobrist_key matches compute_zobrist_key (the hash is
///     computed via the abstract `Board&` interface so the
///     mailbox and bitboard implementations *must* agree);
///   * structural equality holds for FEN round-trip.
#include "board/board8x8_mailbox.hpp"
#include "board/board_bitboard.hpp"

#include <chesserazade/bitboard.hpp>
#include <chesserazade/fen.hpp>
#include <chesserazade/zobrist.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace chesserazade;

namespace {

BoardBitboard bb_from(std::string_view fen) {
    auto r = BoardBitboard::from_fen(fen);
    REQUIRE(r.has_value());
    return *r;
}
Board8x8Mailbox mb_from(std::string_view fen) {
    auto r = Board8x8Mailbox::from_fen(fen);
    REQUIRE(r.has_value());
    return *r;
}

void require_piece_at_agrees(const Board& a, const Board& b) {
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Square s = static_cast<Square>(i);
        REQUIRE(a.piece_at(s) == b.piece_at(s));
    }
}

} // namespace

TEST_CASE("BoardBitboard: from_fen round-trips the starting position",
          "[board_bb]") {
    auto bb = bb_from(std::string{STARTING_POSITION_FEN});
    REQUIRE(serialize_fen(bb) == std::string{STARTING_POSITION_FEN});
}

TEST_CASE("BoardBitboard: piece_at agrees with mailbox on start",
          "[board_bb]") {
    auto bb = bb_from(std::string{STARTING_POSITION_FEN});
    auto mb = mb_from(std::string{STARTING_POSITION_FEN});
    require_piece_at_agrees(bb, mb);
}

TEST_CASE("BoardBitboard: piece_at agrees with mailbox on Kiwipete",
          "[board_bb]") {
    const std::string fen =
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
    auto bb = bb_from(fen);
    auto mb = mb_from(fen);
    require_piece_at_agrees(bb, mb);
}

TEST_CASE("BoardBitboard: zobrist key equals mailbox zobrist",
          "[board_bb][zobrist]") {
    const std::string fen =
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
    auto bb = bb_from(fen);
    auto mb = mb_from(fen);
    REQUIRE(bb.zobrist_key() == mb.zobrist_key());
    // And both agree with the from-scratch recomputation.
    REQUIRE(bb.zobrist_key() == compute_zobrist_key(bb));
    REQUIRE(mb.zobrist_key() == compute_zobrist_key(mb));
}

TEST_CASE("BoardBitboard: occupancy accessors agree with piece_at",
          "[board_bb]") {
    const std::string fen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    auto bb = bb_from(fen);

    // All ranks 1,2,7,8 should be occupied; 3..6 empty.
    REQUIRE((bb.occupancy() & RANK_1) == RANK_1);
    REQUIRE((bb.occupancy() & RANK_2) == RANK_2);
    REQUIRE((bb.occupancy() & RANK_7) == RANK_7);
    REQUIRE((bb.occupancy() & RANK_8) == RANK_8);
    REQUIRE((bb.occupancy() & (RANK_3 | RANK_4 | RANK_5 | RANK_6)) == 0);

    REQUIRE((bb.color_occupancy(Color::White) & RANK_1) == RANK_1);
    REQUIRE((bb.color_occupancy(Color::White) & RANK_2) == RANK_2);
    REQUIRE((bb.color_occupancy(Color::Black) & RANK_7) == RANK_7);
    REQUIRE((bb.color_occupancy(Color::Black) & RANK_8) == RANK_8);

    // Specific piece set: white knights are on b1 and g1.
    const Bitboard wn = bb.pieces(Color::White, PieceType::Knight);
    REQUIRE(popcount(wn) == 2);
    REQUIRE(contains(wn, Square::B1));
    REQUIRE(contains(wn, Square::G1));
}

TEST_CASE("BoardBitboard: structural equality holds across FEN round-trip",
          "[board_bb]") {
    const std::string fen =
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
    auto a = bb_from(fen);
    auto b = bb_from(serialize_fen(a));
    REQUIRE(a == b);
}

TEST_CASE("BoardBitboard: set_piece_at overwrites cleanly", "[board_bb]") {
    BoardBitboard b;
    b.clear();
    b.set_piece_at(Square::E4, Piece{PieceType::Queen, Color::White});
    REQUIRE(b.piece_at(Square::E4) ==
            (Piece{PieceType::Queen, Color::White}));
    // Overwrite with a different piece type — no double-counting.
    b.set_piece_at(Square::E4, Piece{PieceType::Rook, Color::Black});
    REQUIRE(b.piece_at(Square::E4) ==
            (Piece{PieceType::Rook, Color::Black}));
    REQUIRE(popcount(b.occupancy()) == 1);
    // Clear.
    b.set_piece_at(Square::E4, Piece::none());
    REQUIRE(b.piece_at(Square::E4).is_none());
    REQUIRE(b.occupancy() == 0);
}

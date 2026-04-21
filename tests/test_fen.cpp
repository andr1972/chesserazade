#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace chesserazade;

namespace {

/// Parse a FEN, require success, return the board.
Board8x8Mailbox parse_ok(std::string_view fen) {
    auto res = Board8x8Mailbox::from_fen(fen);
    REQUIRE(res.has_value());
    return *res;
}

/// A set of FEN strings we expect to round-trip byte-for-byte.
constexpr std::array<std::string_view, 6> PERFT_POSITIONS{
    // The standard perft positions; we do not run perft yet but FEN
    // parsing must already accept them so 0.2 can hook in cleanly.
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    // Kiwipete
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2pP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
};

} // namespace

TEST_CASE("initial position parses and round-trips", "[fen]") {
    const auto b = parse_ok(STARTING_POSITION_FEN);

    REQUIRE(b.piece_at(Square::A1) == Piece{PieceType::Rook, Color::White});
    REQUIRE(b.piece_at(Square::E1) == Piece{PieceType::King, Color::White});
    REQUIRE(b.piece_at(Square::E8) == Piece{PieceType::King, Color::Black});
    REQUIRE(b.piece_at(Square::D1) == Piece{PieceType::Queen, Color::White});
    REQUIRE(b.piece_at(Square::D8) == Piece{PieceType::Queen, Color::Black});
    // All middle ranks empty.
    for (int r = 2; r <= 5; ++r) {
        for (int f = 0; f < 8; ++f) {
            const auto sq = make_square(static_cast<File>(f),
                                        static_cast<Rank>(r));
            REQUIRE(b.piece_at(sq).is_none());
        }
    }
    REQUIRE(b.side_to_move() == Color::White);
    const auto cr = b.castling_rights();
    REQUIRE(cr.white_king_side);
    REQUIRE(cr.white_queen_side);
    REQUIRE(cr.black_king_side);
    REQUIRE(cr.black_queen_side);
    REQUIRE(b.en_passant_square() == Square::None);
    REQUIRE(b.halfmove_clock() == 0);
    REQUIRE(b.fullmove_number() == 1);

    REQUIRE(serialize_fen(b) == STARTING_POSITION_FEN);
}

TEST_CASE("perft position FENs round-trip byte-for-byte", "[fen][perft]") {
    for (std::string_view fen : PERFT_POSITIONS) {
        CAPTURE(fen);
        const auto b = parse_ok(fen);
        REQUIRE(serialize_fen(b) == fen);
    }
}

TEST_CASE("black-to-move with en-passant target round-trips", "[fen][ep]") {
    // After 1. e4 — white just pushed a pawn, ep square is e3.
    constexpr std::string_view fen =
        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1";
    const auto b = parse_ok(fen);
    REQUIRE(b.side_to_move() == Color::Black);
    REQUIRE(b.en_passant_square() == Square::E3);
    REQUIRE(serialize_fen(b) == fen);
}

TEST_CASE("no-castling position round-trips with '-'", "[fen][castling]") {
    constexpr std::string_view fen = "4k3/8/8/8/8/8/8/4K3 w - - 0 1";
    const auto b = parse_ok(fen);
    REQUIRE_FALSE(b.castling_rights().any());
    REQUIRE(serialize_fen(b) == fen);
}

TEST_CASE("partial castling rights round-trip in canonical KQkq order", "[fen][castling]") {
    constexpr std::string_view fen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w Kq - 0 1";
    const auto b = parse_ok(fen);
    REQUIRE(b.castling_rights().white_king_side);
    REQUIRE_FALSE(b.castling_rights().white_queen_side);
    REQUIRE_FALSE(b.castling_rights().black_king_side);
    REQUIRE(b.castling_rights().black_queen_side);
    REQUIRE(serialize_fen(b) == fen);
}

TEST_CASE("non-canonical castling order is accepted and canonicalized", "[fen][castling]") {
    // Some FEN emitters write castling in non-canonical order; we
    // accept any permutation on input and emit KQkq on output.
    constexpr std::string_view in  = "4k3/8/8/8/8/8/8/4K3 w qKQk - 0 1";
    constexpr std::string_view out = "4k3/8/8/8/8/8/8/4K3 w KQkq - 0 1";
    const auto b = parse_ok(in);
    REQUIRE(serialize_fen(b) == out);
}

TEST_CASE("FEN parsing rejects malformed inputs", "[fen][error]") {
    struct Case {
        std::string_view fen;
        std::string_view substr;
    };
    constexpr std::array<Case, 13> cases{{
        {"", "6 whitespace-separated fields"},
        {"too few", "6 whitespace-separated fields"},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP w KQkq - 0 1",
         "7 ranks"},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR/8 w KQkq - 0 1",
         "more than 8 ranks"},
        {"rnbqkbnr/ppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
         "files"},
        {"rnbqkbnr/pppppppX/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
         "unexpected character"},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1",
         "expected 'w' or 'b'"},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KXkq - 0 1",
         "invalid character"},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e4 0 1",
         "rank 3 or 6"},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq z3 0 1",
         "algebraic square"},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - -1 1",
         "non-negative integer"},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 0",
         "at least 1"},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KKkq - 0 1",
         "duplicate 'K'"},
    }};
    for (const auto& c : cases) {
        CAPTURE(c.fen);
        const auto res = Board8x8Mailbox::from_fen(c.fen);
        REQUIRE_FALSE(res.has_value());
        const std::string msg = res.error().message;
        CAPTURE(msg);
        REQUIRE(msg.find(std::string{c.substr}) != std::string::npos);
    }
}

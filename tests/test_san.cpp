/// Tests for SAN I/O.
///
/// We test parse and write together on the same positions — a
/// `parse -> write` round-trip catches most mistakes, and a few
/// stand-alone cases cover what round-trip alone misses
/// (annotation stripping, digit-castle aliases, explicit
/// ambiguity errors).
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/move_generator.hpp>
#include <chesserazade/san.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace chesserazade;

namespace {

Board8x8Mailbox board_from(std::string_view fen) {
    auto r = Board8x8Mailbox::from_fen(fen);
    REQUIRE(r.has_value());
    return *r;
}

/// Parse then write on a fresh board; the written SAN must equal
/// `expected_written` (which may differ from `input_san` when the
/// input used a tolerated alias like "0-0").
void check_round_trip(std::string_view fen, std::string_view input_san,
                      std::string_view expected_written) {
    auto bp = board_from(fen);
    auto m = parse_san(bp, input_san);
    REQUIRE(m.has_value());
    auto bw = board_from(fen);
    const std::string written = to_san(bw, *m);
    REQUIRE(written == expected_written);
}

} // namespace

// ---------------------------------------------------------------------------
// Pawn moves
// ---------------------------------------------------------------------------

TEST_CASE("SAN: pawn push from the starting position", "[san][pawn]") {
    check_round_trip(std::string{STARTING_POSITION_FEN}, "e4", "e4");
    check_round_trip(std::string{STARTING_POSITION_FEN}, "e3", "e3");
}

TEST_CASE("SAN: pawn capture uses from-file 'exd5'", "[san][pawn]") {
    // Black pawn on d5, white pawn on e4. White plays exd5.
    check_round_trip("rnbqkbnr/ppp1pppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
                     "exd5", "exd5");
}

TEST_CASE("SAN: en-passant round-trips as a normal pawn capture", "[san][ep]") {
    // After 1. e4 d5 2. e5 f5 — white to play, en-passant target on f6.
    check_round_trip("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
                     "exf6", "exf6");
}

TEST_CASE("SAN: promotion without capture 'e8=Q'", "[san][promotion]") {
    // White pawn on e7 promoting on empty e8. Black king sits on a3
    // where no promoted piece attacks it — the suffix stays bare so
    // we're testing the promotion mechanics, not check detection.
    const std::string fen = "8/4P3/8/8/8/k7/8/7K w - - 0 1";
    check_round_trip(fen, "e8=Q", "e8=Q");
    check_round_trip(fen, "e8=N", "e8=N");
}

TEST_CASE("SAN: promotion with capture 'exd8=Q'", "[san][promotion]") {
    // White pawn on e7, black rook on d8, king on f8. Promo-capture on d8.
    check_round_trip("3r1k2/4P3/8/8/8/8/8/4K3 w - - 0 1", "exd8=Q", "exd8=Q+");
}

// ---------------------------------------------------------------------------
// Piece moves and disambiguation
// ---------------------------------------------------------------------------

TEST_CASE("SAN: knight move 'Nf3'", "[san][knight]") {
    check_round_trip(std::string{STARTING_POSITION_FEN}, "Nf3", "Nf3");
}

TEST_CASE("SAN: knight file disambiguation 'Nbd7'", "[san][disambig]") {
    // Two black knights can go to d7: one on b8 (-1,+2) and one on
    // f6 (+1,-2). File disambiguation is enough (ranks differ).
    const std::string fen = "rn2k3/ppp5/5n2/8/8/8/8/4K3 b - - 0 1";
    check_round_trip(fen, "Nbd7", "Nbd7");
    check_round_trip(fen, "Nfd7", "Nfd7");
}

TEST_CASE("SAN: knight rank disambiguation 'N1a3'/'N5a3' when files collide",
          "[san][disambig]") {
    // White knights on b1 and b5 both reach a3 (from b1: +2,-1;
    // from b5: -2,-1). Same file (b) -> rank disambiguation.
    const std::string fen = "4k3/8/8/1N6/8/8/8/1N2K3 w - - 0 1";
    check_round_trip(fen, "N1a3", "N1a3");
    check_round_trip(fen, "N5a3", "N5a3");
}

TEST_CASE("SAN: minimal disambiguation prefers rank/file over full square",
          "[san][disambig]") {
    // Three white queens on a1, c1, a3 can all reach b2.
    //   Qa3 -> unique rank (3)        -> "Q3b2"
    //   Qa1 -> file and rank both collide -> full "Qa1b2"
    //   Qc1 -> unique file (c)        -> "Qcb2"
    // The PGN standard mandates the shortest that disambiguates.
    const std::string fen = "4k3/8/8/8/8/Q7/8/Q1Q1K3 w - - 0 1";
    check_round_trip(fen, "Q3b2",  "Q3b2");
    check_round_trip(fen, "Qa3b2", "Q3b2");  // over-specified input accepted
    check_round_trip(fen, "Qa1b2", "Qa1b2");
    check_round_trip(fen, "Qcb2",  "Qcb2");
    check_round_trip(fen, "Qc1b2", "Qcb2");
}

TEST_CASE("SAN: no disambiguation when piece is unique", "[san][disambig]") {
    // Lone white knight on e4. e4->f6 also happens to give check,
    // so the written form is "Nf6+".
    const std::string fen = "4k3/8/8/8/4N3/8/8/4K3 w - - 0 1";
    check_round_trip(fen, "Nf6", "Nf6+");
    // Spurious 'x' on a non-capture is tolerated on input.
    check_round_trip(fen, "Nxf6", "Nf6+");
}

// ---------------------------------------------------------------------------
// Castling
// ---------------------------------------------------------------------------

TEST_CASE("SAN: kingside castling round-trips as 'O-O'", "[san][castle]") {
    // White king on e1, rook on h1; all rights; squares clear. After
    // O-O the white rook lands on f1 — it does not attack e8, so no
    // check suffix.
    const std::string fen = "4k3/8/8/8/8/8/8/4K2R w K - 0 1";
    check_round_trip(fen, "O-O", "O-O");
}

TEST_CASE("SAN: queenside castling round-trips as 'O-O-O'", "[san][castle]") {
    const std::string fen = "4k3/8/8/8/8/8/8/R3K3 w Q - 0 1";
    check_round_trip(fen, "O-O-O", "O-O-O");
}

TEST_CASE("SAN: digit-castle '0-0' / '0-0-0' are accepted on input",
          "[san][castle]") {
    const std::string fen_ks = "4k3/8/8/8/8/8/8/4K2R w K - 0 1";
    check_round_trip(fen_ks, "0-0", "O-O");
    const std::string fen_qs = "4k3/8/8/8/8/8/8/R3K3 w Q - 0 1";
    check_round_trip(fen_qs, "0-0-0", "O-O-O");
}

// ---------------------------------------------------------------------------
// Check and mate
// ---------------------------------------------------------------------------

TEST_CASE("SAN: plain check gets '+' suffix", "[san][check]") {
    // Black king on h8, white queen on d1 (no line of attack yet).
    // After Qd4 the queen sits on the d4-h8 diagonal and delivers
    // check along it; black still has legal replies (Kg7 etc.).
    const std::string fen = "7k/8/8/8/8/8/8/3QK3 w - - 0 1";
    check_round_trip(fen, "Qd4", "Qd4+");
    // Sanity: a move that doesn't give check stays bare.
    check_round_trip(fen, "Qd3", "Qd3");
}

TEST_CASE("SAN: mate gets '#' suffix", "[san][mate]") {
    // Textbook back-rank mate: black king on g8 walled in by its
    // own pawns on f7/g7/h7, white rook swings to a8. No escape,
    // no interposition, no capture -> '#'.
    const std::string fen = "6k1/5ppp/8/8/8/8/8/R6K w - - 0 1";
    check_round_trip(fen, "Ra8", "Ra8#");
}

// ---------------------------------------------------------------------------
// Annotations
// ---------------------------------------------------------------------------

TEST_CASE("SAN: trailing '+', '#', '!', '?' are tolerated on input",
          "[san][annotation]") {
    // Start from initial. "e4+", "e4!", "e4?!", "e4!!" must all parse.
    Board8x8Mailbox b = board_from(std::string{STARTING_POSITION_FEN});
    for (std::string_view s : {"e4", "e4+", "e4!", "e4?", "e4!!", "e4?!"}) {
        auto r = parse_san(b, s);
        REQUIRE(r.has_value());
    }
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

TEST_CASE("SAN: empty string is rejected", "[san][error]") {
    Board8x8Mailbox b = board_from(std::string{STARTING_POSITION_FEN});
    REQUIRE_FALSE(parse_san(b, "").has_value());
}

TEST_CASE("SAN: illegal move is rejected", "[san][error]") {
    // From the starting position, "Qh5" is not legal — the queen is
    // on d1 and h5 is blocked by nothing but the queen can't reach
    // it (d1-h5 is not a queen line).
    Board8x8Mailbox b = board_from(std::string{STARTING_POSITION_FEN});
    auto r = parse_san(b, "Qh5");
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("SAN: invalid promotion letter is rejected", "[san][error]") {
    Board8x8Mailbox b = board_from("4k3/4P3/8/8/8/8/8/4K3 w - - 0 1");
    REQUIRE_FALSE(parse_san(b, "e8=K").has_value());
    REQUIRE_FALSE(parse_san(b, "e8=P").has_value());
}

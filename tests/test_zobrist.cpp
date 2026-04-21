/// Tests for Zobrist keys and their incremental update in
/// `make_move` / `unmake_move`.
///
/// The two properties that matter for a transposition table:
///   1. The incremental key equals what `compute_zobrist_key`
///      produces when run from scratch on the same position —
///      after any number of moves.
///   2. Transpositions collide. `1.e4 c5 2.Nf3` and `1.Nf3 c5
///      2.e4` reach the same position and must share a key.
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/move_generator.hpp>
#include <chesserazade/zobrist.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace chesserazade;

namespace {

Board8x8Mailbox board_from(std::string_view fen) {
    auto r = Board8x8Mailbox::from_fen(fen);
    REQUIRE(r.has_value());
    return *r;
}

Move find_move(Board& b, Square from, Square to) {
    const MoveList ml = MoveGenerator::generate_legal(b);
    for (const Move& m : ml) {
        if (m.from == from && m.to == to) return m;
    }
    FAIL("expected move not generated");
    return {};
}

} // namespace

TEST_CASE("Zobrist: starting-position key matches from-scratch", "[zobrist]") {
    auto b = board_from(std::string{STARTING_POSITION_FEN});
    REQUIRE(b.zobrist_key() == compute_zobrist_key(b));
    REQUIRE(b.zobrist_key() != 0);
}

TEST_CASE("Zobrist: incremental update equals from-scratch after a move",
          "[zobrist]") {
    auto b = board_from(std::string{STARTING_POSITION_FEN});
    const Move e4 = find_move(b, Square::E2, Square::E4);
    b.make_move(e4);
    REQUIRE(b.zobrist_key() == compute_zobrist_key(b));
}

TEST_CASE("Zobrist: make/unmake restores the key exactly", "[zobrist]") {
    auto b = board_from(std::string{STARTING_POSITION_FEN});
    const ZobristKey before = b.zobrist_key();
    const Move e4 = find_move(b, Square::E2, Square::E4);
    b.make_move(e4);
    b.unmake_move(e4);
    REQUIRE(b.zobrist_key() == before);
}

TEST_CASE("Zobrist: transposition via knight moves", "[zobrist]") {
    // 1.Nf3 Nc6  2.Nc3 Nf6   ends at the same position as
    // 1.Nc3 Nf6  2.Nf3 Nc6.
    //
    // We use only knight moves so that neither sequence ever sets
    // the en-passant square — a pawn double-push at any point
    // would make the two positions differ in EP state (Zobrist
    // is order-sensitive about EP even when pieces agree).
    auto a = board_from(std::string{STARTING_POSITION_FEN});
    a.make_move(find_move(a, Square::G1, Square::F3));
    a.make_move(find_move(a, Square::B8, Square::C6));
    a.make_move(find_move(a, Square::B1, Square::C3));
    a.make_move(find_move(a, Square::G8, Square::F6));

    auto c = board_from(std::string{STARTING_POSITION_FEN});
    c.make_move(find_move(c, Square::B1, Square::C3));
    c.make_move(find_move(c, Square::G8, Square::F6));
    c.make_move(find_move(c, Square::G1, Square::F3));
    c.make_move(find_move(c, Square::B8, Square::C6));

    REQUIRE(a.zobrist_key() == c.zobrist_key());
}

TEST_CASE("Zobrist: different positions get different keys", "[zobrist]") {
    auto a = board_from(std::string{STARTING_POSITION_FEN});
    auto b = board_from(std::string{STARTING_POSITION_FEN});
    const Move e4 = find_move(b, Square::E2, Square::E4);
    b.make_move(e4);
    REQUIRE(a.zobrist_key() != b.zobrist_key());
}

TEST_CASE("Zobrist: side-to-move changes the key", "[zobrist]") {
    auto w = board_from("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    auto b = board_from("4k3/8/8/8/8/8/8/4K3 b - - 0 1");
    REQUIRE(w.zobrist_key() != b.zobrist_key());
}

TEST_CASE("Zobrist: castling rights change the key", "[zobrist]") {
    auto full  = board_from("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
    auto fewer = board_from("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQk - 0 1");
    REQUIRE(full.zobrist_key() != fewer.zobrist_key());
}

TEST_CASE("Zobrist: key stays in sync across a perft-style walk",
          "[zobrist]") {
    // Walk every legal move at depth 2, recursively, and verify
    // the incremental key always matches the from-scratch key.
    auto b = board_from(std::string{STARTING_POSITION_FEN});

    const MoveList ml = MoveGenerator::generate_legal(b);
    for (const Move& m : ml) {
        b.make_move(m);
        REQUIRE(b.zobrist_key() == compute_zobrist_key(b));

        const MoveList inner = MoveGenerator::generate_legal(b);
        for (const Move& m2 : inner) {
            b.make_move(m2);
            REQUIRE(b.zobrist_key() == compute_zobrist_key(b));
            b.unmake_move(m2);
            REQUIRE(b.zobrist_key() == compute_zobrist_key(b));
        }

        b.unmake_move(m);
        REQUIRE(b.zobrist_key() == compute_zobrist_key(b));
    }
}

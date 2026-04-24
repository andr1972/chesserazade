// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Tests for `Game` — a `Board` plus move history.
///
/// Coverage: default / FEN construction, play + undo round-trip,
/// multi-move sequences, reset_to_start.
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/game.hpp>
#include <chesserazade/move_generator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace chesserazade;

namespace {

/// Construct a quiet e2-e4 by looking it up in the legal move list.
/// Tests should not hand-craft `Move` objects — the generator's
/// MoveKind / moved_piece wiring is part of what we're testing.
Move find_move(Board& b, Square from, Square to) {
    const MoveList ml = MoveGenerator::generate_legal(b);
    for (const Move& m : ml) {
        if (m.from == from && m.to == to) {
            return m;
        }
    }
    FAIL("expected move not generated");
    return {};
}

} // namespace

TEST_CASE("Game default starts from the standard position", "[game]") {
    Game g;
    REQUIRE(g.ply_count() == 0);
    REQUIRE(g.moves().empty());

    // The starting position should FEN-serialize back to the
    // canonical starting FEN. This doubles as a smoke test that the
    // Board& accessor returns the same thing as starting_position().
    REQUIRE(serialize_fen(g.starting_position())
            == std::string{STARTING_POSITION_FEN});
    REQUIRE(serialize_fen(g.current_position())
            == std::string{STARTING_POSITION_FEN});
}

TEST_CASE("Game play + undo restores the starting position", "[game]") {
    Game g;
    const std::string before = serialize_fen(g.current_position());

    const Move e4 = find_move(g.current_position(), Square::E2, Square::E4);
    g.play_move(e4);

    REQUIRE(g.ply_count() == 1);
    REQUIRE(g.moves().front() == e4);
    REQUIRE(serialize_fen(g.current_position()) != before);

    REQUIRE(g.undo_move());
    REQUIRE(g.ply_count() == 0);
    REQUIRE(serialize_fen(g.current_position()) == before);
}

TEST_CASE("Game undo on empty history returns false", "[game]") {
    Game g;
    REQUIRE_FALSE(g.undo_move());
    REQUIRE(g.ply_count() == 0);
}

TEST_CASE("Game plays a four-ply sequence and undoes it fully", "[game]") {
    Game g;
    const std::string initial = serialize_fen(g.current_position());

    // 1. e4 e5  2. Nf3 Nc6
    const Move e4 = find_move(g.current_position(), Square::E2, Square::E4);
    g.play_move(e4);
    const Move e5 = find_move(g.current_position(), Square::E7, Square::E5);
    g.play_move(e5);
    const Move Nf3 = find_move(g.current_position(), Square::G1, Square::F3);
    g.play_move(Nf3);
    const Move Nc6 = find_move(g.current_position(), Square::B8, Square::C6);
    g.play_move(Nc6);

    REQUIRE(g.ply_count() == 4);
    REQUIRE(g.moves()[0] == e4);
    REQUIRE(g.moves()[3] == Nc6);

    while (g.undo_move()) {
        // walk history back to the start
    }
    REQUIRE(g.ply_count() == 0);
    REQUIRE(serialize_fen(g.current_position()) == initial);
}

TEST_CASE("Game constructed from a non-standard FEN remembers it", "[game]") {
    // Kiwipete — the classical "many-special-moves" perft position.
    const std::string kiwipete =
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
    auto board = Board8x8Mailbox::from_fen(kiwipete);
    REQUIRE(board.has_value());

    Game g(*board);
    REQUIRE(serialize_fen(g.starting_position()) == kiwipete);
    REQUIRE(serialize_fen(g.current_position()) == kiwipete);
    REQUIRE(g.ply_count() == 0);
}

TEST_CASE("Game reset_to_start drops history and restores start", "[game]") {
    Game g;
    const Move e4 = find_move(g.current_position(), Square::E2, Square::E4);
    g.play_move(e4);
    const Move e5 = find_move(g.current_position(), Square::E7, Square::E5);
    g.play_move(e5);
    REQUIRE(g.ply_count() == 2);

    g.reset_to_start();
    REQUIRE(g.ply_count() == 0);
    REQUIRE(g.moves().empty());
    REQUIRE(serialize_fen(g.current_position())
            == std::string{STARTING_POSITION_FEN});
}

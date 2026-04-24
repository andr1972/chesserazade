// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Tests for PGN I/O.
///
/// Coverage:
///   * Tag block parse.
///   * Move text parse (plain, with comments, with variations,
///     with NAGs, with move numbers written "1.", "1...").
///   * Termination marker parse.
///   * Non-standard starting position via SetUp / FEN tags.
///   * Round-trip: PGN -> PgnGame -> Game -> write_pgn -> parse again.
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/game.hpp>
#include <chesserazade/pgn.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace chesserazade;

namespace {

/// Build a `Game` from a parsed PGN (replaying its move list from
/// the parsed starting FEN). Used on both sides of round-trip
/// tests.
Game game_from_parsed(const PgnGame& p) {
    Board8x8Mailbox start;
    if (p.starting_fen) {
        auto r = Board8x8Mailbox::from_fen(*p.starting_fen);
        REQUIRE(r.has_value());
        start = *r;
    } else {
        auto r = Board8x8Mailbox::from_fen(std::string{STARTING_POSITION_FEN});
        REQUIRE(r.has_value());
        start = *r;
    }
    Game g(std::move(start));
    for (const Move& m : p.moves) {
        g.play_move(m);
    }
    return g;
}

} // namespace

// ---------------------------------------------------------------------------
// Tag parsing
// ---------------------------------------------------------------------------

TEST_CASE("PGN: parses the Seven-Tag Roster", "[pgn][tags]") {
    const std::string pgn =
        "[Event \"Casual Game\"]\n"
        "[Site \"Internet\"]\n"
        "[Date \"2025.01.01\"]\n"
        "[Round \"-\"]\n"
        "[White \"Alice\"]\n"
        "[Black \"Bob\"]\n"
        "[Result \"*\"]\n"
        "\n"
        "*\n";
    auto r = parse_pgn(pgn);
    REQUIRE(r.has_value());
    REQUIRE(r->tags.size() == 7);
    REQUIRE(r->tag("Event").value() == "Casual Game");
    REQUIRE(r->tag("White").value() == "Alice");
    REQUIRE(r->tag("Black").value() == "Bob");
    REQUIRE(r->termination == "*");
    REQUIRE(r->moves.empty());
}

TEST_CASE("PGN: escaped quotes in a tag value survive parsing",
          "[pgn][tags]") {
    const std::string pgn = "[Event \"\\\"Rapid\\\"\"]\n\n*\n";
    auto r = parse_pgn(pgn);
    REQUIRE(r.has_value());
    REQUIRE(r->tag("Event").value() == "\"Rapid\"");
}

// ---------------------------------------------------------------------------
// Move text
// ---------------------------------------------------------------------------

TEST_CASE("PGN: parses a short opening", "[pgn][moves]") {
    const std::string pgn =
        "[Event \"t\"] [Site \"t\"] [Date \"t\"] [Round \"t\"]\n"
        "[White \"t\"] [Black \"t\"] [Result \"*\"]\n"
        "\n"
        "1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 *\n";
    auto r = parse_pgn(pgn);
    REQUIRE(r.has_value());
    REQUIRE(r->moves.size() == 6);
    REQUIRE(r->termination == "*");
}

TEST_CASE("PGN: comments and NAGs are ignored but moves are kept",
          "[pgn][moves]") {
    const std::string pgn =
        "[Event \"t\"] [Site \"t\"] [Date \"t\"] [Round \"t\"]\n"
        "[White \"t\"] [Black \"t\"] [Result \"*\"]\n"
        "\n"
        "1. e4 {This is a great move.} e5 $10 ; a line comment\n"
        "2. Nf3 Nc6 *\n";
    auto r = parse_pgn(pgn);
    REQUIRE(r.has_value());
    REQUIRE(r->moves.size() == 4);
}

TEST_CASE("PGN: variations are skipped", "[pgn][variations]") {
    const std::string pgn =
        "[Event \"t\"] [Site \"t\"] [Date \"t\"] [Round \"t\"]\n"
        "[White \"t\"] [Black \"t\"] [Result \"*\"]\n"
        "\n"
        "1. e4 (1. d4 d5) e5 2. Nf3 *\n";
    auto r = parse_pgn(pgn);
    REQUIRE(r.has_value());
    REQUIRE(r->moves.size() == 3); // e4, e5, Nf3 — variation dropped
}

TEST_CASE("PGN: termination markers are recognized", "[pgn][termination]") {
    constexpr const char* termini[] = {"1-0", "0-1", "1/2-1/2", "*"};
    for (const char* term : termini) {
        const std::string pgn =
            std::string{"[Event \"t\"][Site \"t\"][Date \"t\"][Round \"t\"]"
                        "[White \"t\"][Black \"t\"][Result \"*\"]\n\n1. e4 "}
            + term + "\n";
        auto r = parse_pgn(pgn);
        REQUIRE(r.has_value());
        REQUIRE(r->termination == term);
    }
}

TEST_CASE("PGN: [SetUp \"1\"] + [FEN] set a non-standard starting position",
          "[pgn][fen]") {
    // Kiwipete as the starting point. We play one move so parsing
    // must have loaded the right board to resolve it.
    const std::string pgn =
        "[Event \"t\"][Site \"t\"][Date \"t\"][Round \"t\"]"
        "[White \"t\"][Black \"t\"][Result \"*\"]"
        "[SetUp \"1\"]"
        "[FEN \"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1\"]"
        "\n\n1. O-O *\n";
    auto r = parse_pgn(pgn);
    REQUIRE(r.has_value());
    REQUIRE(r->starting_fen.has_value());
    REQUIRE(r->moves.size() == 1);
    // Castling is MoveKind 2 (KingsideCastle).
    REQUIRE(r->moves.front().kind == MoveKind::KingsideCastle);
}

TEST_CASE("PGN: malformed input yields an error", "[pgn][error]") {
    // Unterminated brace comment.
    auto r1 = parse_pgn("[Event \"t\"][Site \"t\"][Date \"t\"][Round \"t\"]"
                        "[White \"t\"][Black \"t\"][Result \"*\"]\n\n"
                        "1. e4 { unterminated\n");
    REQUIRE_FALSE(r1.has_value());

    // Illegal SAN against the starting position.
    auto r2 = parse_pgn("[Event \"t\"][Site \"t\"][Date \"t\"][Round \"t\"]"
                        "[White \"t\"][Black \"t\"][Result \"*\"]\n\n"
                        "1. Qh5 *\n");
    REQUIRE_FALSE(r2.has_value());
}

// ---------------------------------------------------------------------------
// Round-trip
// ---------------------------------------------------------------------------

TEST_CASE("PGN: write_pgn produces the Seven-Tag Roster block", "[pgn][write]") {
    Game g;
    const std::vector<std::pair<std::string, std::string>> tags = {
        {"Event", "Casual"},
        {"Site",  "Here"},
        {"Date",  "2025.01.02"},
        {"Round", "-"},
        {"White", "Alice"},
        {"Black", "Bob"},
        {"Result","*"},
    };
    const std::string out = write_pgn(g, tags, "*");
    // STR block appears in canonical order and ends with the
    // Result (== termination) tag before the blank line.
    REQUIRE(out.find("[Event \"Casual\"]") != std::string::npos);
    REQUIRE(out.find("[Result \"*\"]") != std::string::npos);
    REQUIRE(out.find("\n\n") != std::string::npos);
}

TEST_CASE("PGN: parse -> play -> write -> parse yields the same moves",
          "[pgn][round-trip]") {
    const std::string original =
        "[Event \"Casual\"][Site \"s\"][Date \"d\"][Round \"r\"]"
        "[White \"w\"][Black \"b\"][Result \"*\"]\n\n"
        "1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 4. Ba4 Nf6 5. O-O Be7 *\n";

    auto parsed1 = parse_pgn(original);
    REQUIRE(parsed1.has_value());

    Game g = game_from_parsed(*parsed1);
    const std::string written = write_pgn(g, parsed1->tags, parsed1->termination);

    auto parsed2 = parse_pgn(written);
    REQUIRE(parsed2.has_value());
    REQUIRE(parsed2->moves == parsed1->moves);
    REQUIRE(parsed2->termination == parsed1->termination);
}

TEST_CASE("PGN: round-trip from a non-standard starting position",
          "[pgn][round-trip][fen]") {
    const std::string original =
        "[Event \"t\"][Site \"t\"][Date \"t\"][Round \"t\"]"
        "[White \"t\"][Black \"t\"][Result \"*\"]"
        "[SetUp \"1\"]"
        "[FEN \"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1\"]"
        "\n\n1. O-O O-O 2. d6 *\n";

    auto parsed1 = parse_pgn(original);
    REQUIRE(parsed1.has_value());
    REQUIRE(parsed1->starting_fen.has_value());

    Game g = game_from_parsed(*parsed1);
    const std::string written = write_pgn(g, parsed1->tags, parsed1->termination);

    auto parsed2 = parse_pgn(written);
    REQUIRE(parsed2.has_value());
    REQUIRE(parsed2->moves == parsed1->moves);
    REQUIRE(parsed2->starting_fen == parsed1->starting_fen);
}

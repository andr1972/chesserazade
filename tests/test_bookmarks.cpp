// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Tests for the bookmarks module. The UI (dialogs, list view)
/// is exercised by hand; here we pin down the parts that matter
/// for correctness across bookmark lifetimes:
///   * ply ↔ notation round-trip (stored as int, shown as "N w"),
///   * fuzzy game resolver (wildcards in dates, substring names,
///     event/round tie-breakers).
#include "../analyzer/bookmarks.hpp"

#include <catch2/catch_test_macros.hpp>

using chesserazade::analyzer::Bookmark;
using chesserazade::analyzer::ply_to_notation;
using chesserazade::analyzer::notation_to_ply;
using chesserazade::analyzer::resolve_game;
using chesserazade::PgnGameHeader;

TEST_CASE("bookmark notation: ply 0 renders as '0' and round-trips",
          "[bookmark]") {
    REQUIRE(ply_to_notation(0) == "0");
    REQUIRE(notation_to_ply("0").value() == 0);
}

TEST_CASE("bookmark notation: 'N w' = 2N-1, 'N b' = 2N",
          "[bookmark]") {
    REQUIRE(ply_to_notation(1)  == "1 w");
    REQUIRE(ply_to_notation(2)  == "1 b");
    REQUIRE(ply_to_notation(33) == "17 w");
    REQUIRE(ply_to_notation(34) == "17 b");

    REQUIRE(notation_to_ply("1 w").value() == 1);
    REQUIRE(notation_to_ply("1 b").value() == 2);
    REQUIRE(notation_to_ply("17 w").value() == 33);
    REQUIRE(notation_to_ply("17 b").value() == 34);
}

TEST_CASE("bookmark notation: case and whitespace tolerant on parse",
          "[bookmark]") {
    REQUIRE(notation_to_ply("17 W").value() == 33);
    REQUIRE(notation_to_ply("  17   b  ").value() == 34);
}

TEST_CASE("bookmark notation: garbage input returns nullopt",
          "[bookmark]") {
    REQUIRE_FALSE(notation_to_ply("").has_value());
    REQUIRE_FALSE(notation_to_ply("w 17").has_value());
    REQUIRE_FALSE(notation_to_ply("17").has_value());
    REQUIRE_FALSE(notation_to_ply("17 x").has_value());
    REQUIRE_FALSE(notation_to_ply("0 w").has_value());
}

namespace {

PgnGameHeader make_hdr(std::string white, std::string black,
                       std::string date, std::string event = "",
                       std::string round = "") {
    PgnGameHeader h;
    h.white = std::move(white);
    h.black = std::move(black);
    h.date  = std::move(date);
    h.event = std::move(event);
    h.round = std::move(round);
    return h;
}

} // namespace

TEST_CASE("resolver: single match on white+black",
          "[bookmark][resolver]") {
    std::vector<PgnGameHeader> h = {
        make_hdr("Fischer, Robert J.", "Spassky, Boris V.",
                 "1972.07.11"),
        make_hdr("Carlsen, Magnus", "Nepomniachtchi, Ian",
                 "2021.11.26"),
    };
    Bookmark bm;
    bm.white = "Fischer";
    bm.black = "Spassky";
    REQUIRE(resolve_game(bm, h).value() == 0);
}

TEST_CASE("resolver: date wildcard — bookmark year-only matches "
          "full-date header", "[bookmark][resolver]") {
    std::vector<PgnGameHeader> h = {
        make_hdr("X", "Y", "1972.07.11"),
        make_hdr("X", "Y", "1973.01.01"),
    };
    Bookmark bm;
    bm.white = "X";
    bm.black = "Y";
    bm.date  = "1972"; // year only
    REQUIRE(resolve_game(bm, h).value() == 0);
}

TEST_CASE("resolver: date wildcard — all-question-marks matches anything",
          "[bookmark][resolver]") {
    std::vector<PgnGameHeader> h = {
        make_hdr("X", "Y", "1972.07.11"),
    };
    Bookmark bm;
    bm.white = "X";
    bm.black = "Y";
    bm.date  = "????.??.??";
    REQUIRE(resolve_game(bm, h).value() == 0);
}

TEST_CASE("resolver: event tie-breaker when same pair played twice",
          "[bookmark][resolver]") {
    std::vector<PgnGameHeader> h = {
        make_hdr("X", "Y", "1972.07.11", "Blitz Match"),
        make_hdr("X", "Y", "1972.07.11", "Classical Match"),
    };
    Bookmark bm;
    bm.white = "X";
    bm.black = "Y";
    bm.date  = "1972.07.11";
    bm.event = "Classical";
    REQUIRE(resolve_game(bm, h).value() == 1);
}

TEST_CASE("resolver: no tie-breaker → nullopt on ambiguity",
          "[bookmark][resolver]") {
    std::vector<PgnGameHeader> h = {
        make_hdr("X", "Y", "1972.07.11"),
        make_hdr("X", "Y", "1972.07.11"),
    };
    Bookmark bm;
    bm.white = "X";
    bm.black = "Y";
    REQUIRE_FALSE(resolve_game(bm, h).has_value());
}

TEST_CASE("resolver: no survivor → nullopt", "[bookmark][resolver]") {
    std::vector<PgnGameHeader> h = {
        make_hdr("X", "Y", "1972.07.11"),
    };
    Bookmark bm;
    bm.white = "Z";
    REQUIRE_FALSE(resolve_game(bm, h).has_value());
}

/// Tests for the lightweight PGN header indexer.
#include <chesserazade/pgn_index.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace chesserazade;

namespace {

constexpr std::string_view ONE_GAME =
    "[Event \"F/S Return Match\"]\n"
    "[Site \"Belgrade, Serbia JUG\"]\n"
    "[Date \"1992.11.04\"]\n"
    "[Round \"29\"]\n"
    "[White \"Fischer, Robert J.\"]\n"
    "[Black \"Spassky, Boris V.\"]\n"
    "[Result \"1/2-1/2\"]\n"
    "\n"
    "1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 1/2-1/2\n";

constexpr std::string_view TWO_GAMES =
    "[Event \"Game One\"]\n"
    "[Site \"A\"]\n"
    "[Date \"2001.01.01\"]\n"
    "[Round \"1\"]\n"
    "[White \"Alice\"]\n"
    "[Black \"Bob\"]\n"
    "[Result \"1-0\"]\n"
    "\n"
    "1. e4 e5 2. Nf3 Nc6 1-0\n"
    "\n"
    "[Event \"Game Two\"]\n"
    "[Site \"B\"]\n"
    "[Date \"2002.02.02\"]\n"
    "[Round \"2\"]\n"
    "[White \"Carol\"]\n"
    "[Black \"Dave\"]\n"
    "[Result \"0-1\"]\n"
    "\n"
    "1. d4 d5 0-1\n";

} // namespace

TEST_CASE("PGN index: single game populates STR fields", "[pgn_index]") {
    const auto games = index_games(ONE_GAME);
    REQUIRE(games.size() == 1);

    const auto& g = games[0];
    REQUIRE(g.event  == "F/S Return Match");
    REQUIRE(g.site   == "Belgrade, Serbia JUG");
    REQUIRE(g.date   == "1992.11.04");
    REQUIRE(g.round  == "29");
    REQUIRE(g.white  == "Fischer, Robert J.");
    REQUIRE(g.black  == "Spassky, Boris V.");
    REQUIRE(g.result == "1/2-1/2");
}

TEST_CASE("PGN index: offset points at the Event header", "[pgn_index]") {
    const auto games = index_games(ONE_GAME);
    REQUIRE(games.size() == 1);
    const auto& g = games[0];
    REQUIRE(ONE_GAME.substr(g.offset, 7) == "[Event ");
}

TEST_CASE("PGN index: length covers through the termination token",
          "[pgn_index]") {
    const auto games = index_games(ONE_GAME);
    REQUIRE(games.size() == 1);
    const auto& g = games[0];
    const std::string_view body = ONE_GAME.substr(g.offset, g.length);
    // Final seven characters of the span must be the termination
    // token "1/2-1/2" (the scan consumes it).
    REQUIRE(body.size() >= 7);
    REQUIRE(body.substr(body.size() - 7) == "1/2-1/2");
}

TEST_CASE("PGN index: ply_count on ONE_GAME is 6 "
          "(3 full moves, half-ply each)", "[pgn_index]") {
    // 1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 1/2-1/2  → six SAN tokens.
    const auto games = index_games(ONE_GAME);
    REQUIRE(games.size() == 1);
    REQUIRE(games[0].ply_count == 6);
}

TEST_CASE("PGN index: ECO tag is captured when present",
          "[pgn_index]") {
    constexpr std::string_view src =
        "[Event \"E\"]\n"
        "[White \"W\"]\n"
        "[Black \"B\"]\n"
        "[Result \"1-0\"]\n"
        "[ECO \"C42\"]\n"
        "\n"
        "1. e4 e5 1-0\n";
    const auto games = index_games(src);
    REQUIRE(games.size() == 1);
    REQUIRE(games[0].eco == "C42");
    REQUIRE(games[0].ply_count == 2);
}

TEST_CASE("PGN index: variations and comments are excluded from ply_count",
          "[pgn_index]") {
    constexpr std::string_view src =
        "[Event \"E\"]\n"
        "[White \"W\"]\n"
        "[Black \"B\"]\n"
        "[Result \"1-0\"]\n"
        "\n"
        "1. e4 {best by test} e5 2. Nf3 (2. Bc4 Nf6) Nc6 1-0\n";
    // Main line: e4 e5 Nf3 Nc6 → 4 plies. The variation
    // "Bc4 Nf6" and the `{best by test}` comment are skipped.
    const auto games = index_games(src);
    REQUIRE(games.size() == 1);
    REQUIRE(games[0].ply_count == 4);
}

TEST_CASE("PGN index: NAGs and move numbers are not counted as plies",
          "[pgn_index]") {
    constexpr std::string_view src =
        "[Event \"E\"]\n"
        "[White \"W\"]\n"
        "[Black \"B\"]\n"
        "[Result \"1-0\"]\n"
        "\n"
        "1. e4 $1 e5 $14 2. Nf3 $2 Nc6 1-0\n";
    const auto games = index_games(src);
    REQUIRE(games.size() == 1);
    REQUIRE(games[0].ply_count == 4);
}

TEST_CASE("PGN index: two games are detected and ordered", "[pgn_index]") {
    const auto games = index_games(TWO_GAMES);
    REQUIRE(games.size() == 2);
    REQUIRE(games[0].white == "Alice");
    REQUIRE(games[0].result == "1-0");
    REQUIRE(games[1].white == "Carol");
    REQUIRE(games[1].result == "0-1");

    // Offsets are strictly increasing.
    REQUIRE(games[0].offset < games[1].offset);
    // First game's span ends at or before the second's offset.
    REQUIRE(games[0].offset + games[0].length <= games[1].offset);
}

TEST_CASE("PGN index: game without explicit termination still indexes",
          "[pgn_index]") {
    constexpr std::string_view src =
        "[Event \"Unfinished\"]\n"
        "[Site \"-\"]\n"
        "[Date \"?\"]\n"
        "[Round \"-\"]\n"
        "[White \"W\"]\n"
        "[Black \"B\"]\n"
        "[Result \"*\"]\n"
        "\n"
        "1. e4 *\n";
    const auto games = index_games(src);
    REQUIRE(games.size() == 1);
    REQUIRE(games[0].event == "Unfinished");
    REQUIRE(games[0].result == "*");
}

TEST_CASE("PGN index: empty input yields no games", "[pgn_index]") {
    REQUIRE(index_games("").empty());
    REQUIRE(index_games("\n\n\n").empty());
    REQUIRE(index_games("not a PGN at all").empty());
}

TEST_CASE("PGN index: missing fields default to empty strings", "[pgn_index]") {
    // An unusual but legal PGN that omits Site/Round (some early
    // databases do this). The indexer should still produce an
    // entry; the absent fields are just empty.
    constexpr std::string_view src =
        "[Event \"Sparse\"]\n"
        "[Date \"1900.01.01\"]\n"
        "[White \"X\"]\n"
        "[Black \"Y\"]\n"
        "[Result \"1-0\"]\n"
        "\n"
        "1. c4 1-0\n";
    const auto games = index_games(src);
    REQUIRE(games.size() == 1);
    REQUIRE(games[0].event == "Sparse");
    REQUIRE(games[0].site.empty());
    REQUIRE(games[0].round.empty());
    REQUIRE(games[0].white == "X");
}

TEST_CASE("PGN index: handles PGN-escaped quotes in values",
          "[pgn_index]") {
    constexpr std::string_view src =
        "[Event \"The \\\"Quoted\\\" Cup\"]\n"
        "[Site \"-\"]\n"
        "[Date \"?\"]\n"
        "[Round \"-\"]\n"
        "[White \"A\"]\n"
        "[Black \"B\"]\n"
        "[Result \"*\"]\n"
        "\n"
        "*\n";
    const auto games = index_games(src);
    REQUIRE(games.size() == 1);
    REQUIRE(games[0].event == "The \"Quoted\" Cup");
}

TEST_CASE("PGN index: scales to synthetic 100-game input", "[pgn_index]") {
    // Build a synthetic multi-game PGN by concatenation. The
    // indexer must return exactly 100 entries with strictly
    // increasing offsets.
    std::string big;
    for (int g = 0; g < 100; ++g) {
        big += "[Event \"G";
        big += std::to_string(g);
        big += "\"]\n";
        big += "[Site \"S\"]\n";
        big += "[Date \"2020.01.01\"]\n";
        big += "[Round \"1\"]\n";
        big += "[White \"W\"]\n";
        big += "[Black \"B\"]\n";
        big += "[Result \"1-0\"]\n\n";
        big += "1. e4 1-0\n\n";
    }
    const auto games = index_games(big);
    REQUIRE(games.size() == 100);
    for (std::size_t i = 1; i < games.size(); ++i) {
        REQUIRE(games[i].offset > games[i - 1].offset);
    }
    REQUIRE(games.front().event == "G0");
    REQUIRE(games.back().event == "G99");
}

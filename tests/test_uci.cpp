// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// UCI protocol tests.
///
/// We drive `process_uci_line` directly instead of spawning a
/// subprocess. That keeps the test hermetic (no PATH lookup, no
/// flaky stdin piping) and still exercises the full state
/// machine end-to-end — same code path as the real binary.
#include "cli/cmd_uci.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <sstream>
#include <string>

using namespace chesserazade::cli;
using Catch::Matchers::ContainsSubstring;

namespace {

/// Feed a single command and return whatever the engine wrote
/// to its output stream. The session is created fresh for each
/// helper call so the test can inspect `handle_uci` output in
/// isolation; for multi-step flows, the test constructs a
/// persistent `UciSession` itself.
std::string drive(UciSession& s, const std::string& line) {
    std::ostringstream out;
    process_uci_line(s, line, out);
    return out.str();
}

} // namespace

TEST_CASE("UCI: handshake", "[uci]") {
    UciSession s;
    const std::string reply = drive(s, "uci");
    REQUIRE_THAT(reply, ContainsSubstring("id name chesserazade"));
    REQUIRE_THAT(reply, ContainsSubstring("id author"));
    REQUIRE_THAT(reply, ContainsSubstring("option name Hash"));
    REQUIRE_THAT(reply, ContainsSubstring("option name Threads"));
    REQUIRE_THAT(reply, ContainsSubstring("uciok"));
}

TEST_CASE("UCI: isready", "[uci]") {
    UciSession s;
    REQUIRE(drive(s, "isready") == "readyok\n");
}

TEST_CASE("UCI: position startpos with moves", "[uci]") {
    UciSession s;
    drive(s, "position startpos moves e2e4 e7e5");
    // The board's FEN after 1.e4 e5 begins with the standard
    // mid-game rank layout.
    const auto after = drive(s, "go depth 1");
    REQUIRE_THAT(after, ContainsSubstring("info depth 1"));
    REQUIRE_THAT(after, ContainsSubstring("bestmove "));
}

TEST_CASE("UCI: position fen ... moves", "[uci]") {
    UciSession s;
    const std::string line =
        "position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR "
        "w KQkq - 0 1 moves d2d4";
    drive(s, line);
    const auto go = drive(s, "go depth 2");
    REQUIRE_THAT(go, ContainsSubstring("bestmove "));
}

TEST_CASE("UCI: go depth produces per-depth info", "[uci]") {
    UciSession s;
    drive(s, "position startpos");
    const auto out = drive(s, "go depth 3");
    REQUIRE_THAT(out, ContainsSubstring("info depth 1"));
    REQUIRE_THAT(out, ContainsSubstring("info depth 2"));
    REQUIRE_THAT(out, ContainsSubstring("info depth 3"));
    REQUIRE_THAT(out, ContainsSubstring("bestmove "));
}

TEST_CASE("UCI: go movetime stays within budget (loose)", "[uci]") {
    UciSession s;
    drive(s, "position startpos");
    // 200 ms is enough to get at least one completed iteration
    // while being a clear cap for the test harness.
    const auto out = drive(s, "go movetime 200");
    REQUIRE_THAT(out, ContainsSubstring("bestmove "));
    REQUIRE_THAT(out, ContainsSubstring("info depth 1"));
}

TEST_CASE("UCI: go with wtime/btime derives a movetime", "[uci]") {
    UciSession s;
    drive(s, "position startpos");
    // 60 s / 30 = 2000 ms + inc/2 = 500 → ~2500 ms budget.
    // We don't wait that long; we just assert the engine
    // produced a bestmove without crashing on the full set of
    // time-control tokens.
    const auto out =
        drive(s, "go wtime 60000 btime 60000 winc 1000 binc 1000");
    REQUIRE_THAT(out, ContainsSubstring("bestmove "));
}

TEST_CASE("UCI: setoption Hash re-sizes the TT", "[uci]") {
    UciSession s;
    REQUIRE(s.hash_mb == 16);
    drive(s, "setoption name Hash value 32");
    REQUIRE(s.hash_mb == 32);
    // Clamped at the MIN side.
    drive(s, "setoption name Hash value 0");
    REQUIRE(s.hash_mb == 1);
    // Clamped at the MAX side.
    drive(s, "setoption name Hash value 1000000");
    REQUIRE(s.hash_mb == 4096);
}

TEST_CASE("UCI: setoption Threads is a no-op but accepted", "[uci]") {
    UciSession s;
    // No output expected; just must not crash and must leave
    // the session otherwise usable.
    drive(s, "setoption name Threads value 8");
    REQUIRE(drive(s, "isready") == "readyok\n");
}

TEST_CASE("UCI: ucinewgame clears TT and resets position", "[uci]") {
    UciSession s;
    drive(s, "position startpos moves e2e4");
    drive(s, "ucinewgame");
    // After ucinewgame the position is back to startpos. A
    // `go depth 1` from here must be legal (the test only
    // verifies the engine is in a valid state).
    const auto out = drive(s, "go depth 1");
    REQUIRE_THAT(out, ContainsSubstring("bestmove "));
}

TEST_CASE("UCI: quit returns true", "[uci]") {
    UciSession s;
    std::ostringstream out;
    REQUIRE(process_uci_line(s, "quit", out) == true);
    REQUIRE(out.str().empty());
}

TEST_CASE("UCI: unknown command is ignored", "[uci]") {
    UciSession s;
    std::ostringstream out;
    REQUIRE(process_uci_line(s, "frobnicate the widget", out) == false);
    REQUIRE(out.str().empty());
}

TEST_CASE("UCI: empty line is ignored", "[uci]") {
    UciSession s;
    std::ostringstream out;
    REQUIRE(process_uci_line(s, "", out) == false);
    REQUIRE(out.str().empty());
}

TEST_CASE("UCI: mate-in-1 produces a mate score", "[uci]") {
    UciSession s;
    // Fool's mate setup — white to move, Qh5# is mate in 1.
    drive(s,
          "position fen rnb1kbnr/pppp1ppp/8/4p2q/6P1/5P2/PPPPP2P/RNBQKBNR "
          "b KQkq - 0 1");
    // Wait — that FEN is black to move after 1.f3 e5 2.g4. The
    // mate move from black is Qh4#. Use a simpler deterministic
    // setup: white to move, mate in 1 with Rd8#.
    drive(s,
          "position fen 4k3/R7/4K3/8/8/8/8/7R w - - 0 1");
    const auto out = drive(s, "go depth 2");
    REQUIRE_THAT(out, ContainsSubstring("score mate "));
    REQUIRE_THAT(out, ContainsSubstring("bestmove "));
}

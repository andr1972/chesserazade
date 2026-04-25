// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Tests for the on-disk game index, focused on the
/// material-sacrifice detector (`MaterialSac` series).
///
/// The flagship fixture is Donald Byrne vs Robert Fischer,
/// New York Rosenwald 1956 — the "Game of the Century" —
/// which exercises every interesting branch of the series
/// detector: dropped knight-for-pawn pre-sac, a queen offer
/// that nets to bishop-for-queen in two plies, a long
/// king-chase with checks bridging captures, and a final
/// recovery wave that pushes black's net advantage past the
/// queen value.
#include <chesserazade/game_index.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

using namespace chesserazade;

namespace {

// Game of the Century, Byrne-Fischer, NY Rosenwald 1956.
// Verbatim move text (no annotations) — the index builder
// runs the full PGN parser over this and replays each move.
constexpr std::string_view BYRNE_FISCHER_1956 =
    "[Event \"NY Rosenwald\"]\n"
    "[Site \"New York\"]\n"
    "[Date \"1956.10.17\"]\n"
    "[Round \"8\"]\n"
    "[White \"Byrne, Donald\"]\n"
    "[Black \"Fischer, Robert J.\"]\n"
    "[Result \"0-1\"]\n"
    "\n"
    "1. Nf3 Nf6 2. c4 g6 3. Nc3 Bg7 4. d4 O-O 5. Bf4 d5 "
    "6. Qb3 dxc4 7. Qxc4 c6 8. e4 Nbd7 9. Rd1 Nb6 "
    "10. Qc5 Bg4 11. Bg5 Na4 12. Qa3 Nxc3 13. bxc3 Nxe4 "
    "14. Bxe7 Qb6 15. Bc4 Nxc3 16. Bc5 Rfe8+ 17. Kf1 Be6 "
    "18. Bxb6 Bxc4+ 19. Kg1 Ne2+ 20. Kf1 Nxd4+ 21. Kg1 Ne2+ "
    "22. Kf1 Nc3+ 23. Kg1 axb6 24. Qb4 Ra4 25. Qxb6 Nxd1 "
    "26. h3 Rxa2 27. Kh2 Nxf2 28. Re1 Rxe1 29. Qd8+ Bf8 "
    "30. Nxe1 Bd5 31. Nf3 Ne4 32. Qb8 b5 33. h4 h5 "
    "34. Ne5 Kg7 35. Kg1 Bc5+ 36. Kf1 Ng3+ 37. Ke1 Bb4+ "
    "38. Kd1 Bb3+ 39. Kc1 Ne2+ 40. Kb1 Nc3+ 41. Kc1 Rc2#"
    " 0-1\n";

GameIndex build(std::string_view pgn) {
    std::atomic<bool> cancel{false};
    return build_index(pgn, /*pgn_mtime=*/0, /*progress=*/{}, cancel);
}

} // namespace

TEST_CASE("game_index: schema and basic build", "[game_index]") {
    const GameIndex idx = build(BYRNE_FISCHER_1956);

    REQUIRE(idx.schema == 10);
    REQUIRE(idx.games.size() == 1);

    const GameRecord& g = idx.games[0];
    REQUIRE(g.header.white == "Byrne, Donald");
    REQUIRE(g.header.black == "Fischer, Robert J.");
    REQUIRE(g.header.result == "0-1");
    REQUIRE(g.hash != 0);                  // SAN replay succeeded
    REQUIRE(g.end_kind == EndKind::Mate);  // 41…Rc2#
}

TEST_CASE("game_index: under-promotion list empty for Fischer 1956",
          "[game_index]") {
    const GameIndex idx = build(BYRNE_FISCHER_1956);
    REQUIRE(idx.games[0].underpromotions.empty());
}

TEST_CASE("game_index: knight forks in Fischer 1956",
          "[game_index]") {
    // The post-Be6 sequence has a string of knight checks
    // (Ne2+, Nxd4+, Nc3+) but the fork condition is
    // narrower — it requires the knight to attack a queen or
    // rook on a separate square as well. The detector should
    // still find at least one true fork in this game.
    const GameIndex idx = build(BYRNE_FISCHER_1956);
    REQUIRE(idx.games[0].knight_fork_plies.size() >= 1);
}

TEST_CASE("game_index: Byrne-Fischer 1956 sacrifice series",
          "[game_index][sac]") {
    const GameIndex idx = build(BYRNE_FISCHER_1956);
    const auto& sacs = idx.games[0].material_sacs;

    // The brilliancy is one connected series — captures plus
    // a king-chase whose checks reset the quiet-streak gate.
    // Series boundary: the post-Nxe1 (move 30 white) lull into
    // moves 31-34 has more than two consecutive quiet plies,
    // so the run that began with Nxc3 (ply 30) terminates
    // there. Anything after the lull and the final Rc2# mate
    // is below threshold.
    REQUIRE(sacs.size() == 1);

    const MaterialSac& s = sacs[0];

    // Peak small group: 18.Bxb6 (white captures queen, +900)
    // and 18...Bxc4+ (black recaptures bishop, -330) net to
    // +570 from white's perspective.
    REQUIRE(s.peak_cp == 570);

    // Net of the whole series, summed verbatim from the
    // user's annotation:
    //   -100 +570 -100 -330 -400 -100 -100 -500 +500 = -560
    REQUIRE(s.net_cp == -560);

    // Biggest piece that fell in the peak small group is the
    // queen taken on 18.Bxb6.
    REQUIRE(s.raw_piece == PieceType::Queen);
    REQUIRE(s.raw_piece_cp == 900);

    // The "click on Sac" target is the ply where the queen
    // physically fell. White's move 18 = ply 35 (1-based).
    REQUIRE(s.ply == 35);
}

TEST_CASE("game_index: pawn-only games yield no sac series",
          "[game_index][sac]") {
    // Two-move pseudo game where only a pawn changes hands.
    // The detector should find no series — the only small
    // group is pawn-level and gets filtered out.
    constexpr std::string_view ONLY_PAWN_TRADE =
        "[Event \"X\"]\n[Site \"\"]\n[Date \"2024.01.01\"]\n"
        "[Round \"1\"]\n[White \"A\"]\n[Black \"B\"]\n"
        "[Result \"1/2-1/2\"]\n\n"
        "1. e4 d5 2. exd5 1/2-1/2\n";
    const GameIndex idx = build(ONLY_PAWN_TRADE);
    REQUIRE(idx.games.size() == 1);
    REQUIRE(idx.games[0].material_sacs.empty());
}

TEST_CASE("game_index: equal R-for-R trade produces no series",
          "[game_index][sac]") {
    // Quick rook trade with no other captures — the small
    // group nets to zero (|sum| < 100), so it's dropped at
    // the noise gate before series formation.
    //   1.e4 e5 2.Nf3 Nc6 3.Bb5 a6 4.Bxc6 dxc6 5.Nxe5 ...
    // Specifically craft a true rook swap: open the a-file,
    // exchange both rooks. We use a custom contrived game.
    constexpr std::string_view EQUAL_TRADE =
        "[Event \"X\"]\n[Site \"\"]\n[Date \"2024.01.01\"]\n"
        "[Round \"1\"]\n[White \"A\"]\n[Black \"B\"]\n"
        "[Result \"1/2-1/2\"]\n\n"
        "1. a4 a5 2. Ra3 Ra6 3. Rxa5 Rxa4 1/2-1/2\n";
    const GameIndex idx = build(EQUAL_TRADE);
    REQUIRE(idx.games.size() == 1);
    // Only small group sums to 500-500 = 0 → dropped. No series.
    REQUIRE(idx.games[0].material_sacs.empty());
}

TEST_CASE("game_index: JSON roundtrip preserves sac series",
          "[game_index]") {
    const GameIndex original = build(BYRNE_FISCHER_1956);
    const std::string path = "/tmp/chesserazade_test_idx.json";
    REQUIRE(save_index(path, original));
    const auto loaded = load_index(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->schema == 10);
    REQUIRE(loaded->games.size() == 1);

    const auto& a = original.games[0].material_sacs;
    const auto& b = loaded->games[0].material_sacs;
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].ply == b[i].ply);
        REQUIRE(a[i].net_cp == b[i].net_cp);
        REQUIRE(a[i].peak_cp == b[i].peak_cp);
        REQUIRE(a[i].raw_piece == b[i].raw_piece);
        REQUIRE(a[i].raw_piece_cp == b[i].raw_piece_cp);
    }
}

/// Tests for the game analyzer.
///
/// Strategy: construct a `Game` that plays a clearly bad move in
/// the middle of an otherwise reasonable opening, analyze, and
/// verify:
///   * the NAG on the bad move is the "??" / "?" tier;
///   * the engine's best line is recorded;
///   * moves that match the engine's top choice carry no NAG;
///   * the annotated PGN parses back and round-trips the played
///     moves.
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/game.hpp>
#include <chesserazade/game_analyzer.hpp>
#include <chesserazade/move_generator.hpp>
#include <chesserazade/pgn.hpp>
#include <chesserazade/san.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

using namespace chesserazade;

namespace {

Move san(Board& b, std::string_view s) {
    auto r = parse_san(b, s);
    REQUIRE(r.has_value());
    return *r;
}

Game build_blunder_game() {
    // 1. e4 e5 2. Bc4 Nc6 3. Qh5 Nf6 4. Qxf7#
    // Classic Scholar's Mate — Black's 3...Nf6?? lets white mate
    // next move. Depth 3 is enough to see this as a blunder.
    Game g;
    g.play_move(san(g.current_position(), "e4"));
    g.play_move(san(g.current_position(), "e5"));
    g.play_move(san(g.current_position(), "Bc4"));
    g.play_move(san(g.current_position(), "Nc6"));
    g.play_move(san(g.current_position(), "Qh5"));
    g.play_move(san(g.current_position(), "Nf6"));
    g.play_move(san(g.current_position(), "Qxf7#"));
    return g;
}

} // namespace

TEST_CASE("Analyzer: Scholar's mate — 3...Nf6 is flagged as a blunder",
          "[analyzer]") {
    Game g = build_blunder_game();
    const GameAnalysis ga = GameAnalyzer::analyze(g, /*depth=*/4);

    REQUIRE(ga.plies.size() == g.moves().size());

    // Index 5 is black's 6th ply (3...Nf6). With depth 4 the
    // engine sees the mating reply Qxf7# two plies later and
    // tags the move as a blunder.
    REQUIRE(ga.plies[5].nag_suffix == "??");
    REQUIRE_FALSE(ga.plies[5].comment.empty());
    REQUIRE_FALSE(ga.plies[5].best_line.empty());
}

TEST_CASE("Analyzer: annotated PGN round-trips the move list",
          "[analyzer][pgn]") {
    Game g = build_blunder_game();
    const GameAnalysis ga = GameAnalyzer::analyze(g, 3);

    std::vector<MoveAnnotation> anns;
    anns.reserve(ga.plies.size());
    for (const MoveAnalysis& p : ga.plies) {
        anns.push_back(MoveAnnotation{p.nag_suffix, p.comment});
    }

    const std::vector<std::pair<std::string, std::string>> tags = {
        {"Event", "t"}, {"Site", "t"}, {"Date", "t"}, {"Round", "t"},
        {"White", "w"}, {"Black", "b"}, {"Result", "1-0"},
    };
    const std::string out = write_pgn(g, tags, "1-0", anns);

    // Parse the annotated PGN back; the move list must be
    // identical, the termination preserved, and our comments
    // tolerated on input.
    auto round = parse_pgn(out);
    REQUIRE(round.has_value());
    REQUIRE(round->moves == g.moves());
    REQUIRE(round->termination == "1-0");
}

TEST_CASE("Analyzer: a move that matches the engine's top pick "
          "carries no annotation", "[analyzer]") {
    // 1. e4 is the engine's top choice at almost any reasonable
    // depth; it must not be flagged.
    Game g;
    g.play_move(san(g.current_position(), "e4"));
    const GameAnalysis ga = GameAnalyzer::analyze(g, 3);
    REQUIRE(ga.plies.size() == 1);
    REQUIRE(ga.plies[0].nag_suffix.empty());
    REQUIRE(ga.plies[0].comment.empty());
}

TEST_CASE("Analyzer: empty game returns empty analysis", "[analyzer]") {
    Game g;
    const GameAnalysis ga = GameAnalyzer::analyze(g, 3);
    REQUIRE(ga.plies.empty());
}

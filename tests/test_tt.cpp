/// Tests for the transposition table and its wiring into Search.
///
/// Two layers:
///   1. TT unit-level — probe returns what was stored; stats
///      increment; replacement prefers higher depth / fresher age.
///   2. Search-level — running `find_best` with vs without a TT
///      on the same position produces the same best move, and
///      the TT run visits strictly fewer nodes.
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/search.hpp>
#include <chesserazade/transposition_table.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace chesserazade;

namespace {

Board8x8Mailbox board_from(std::string_view fen) {
    auto r = Board8x8Mailbox::from_fen(fen);
    REQUIRE(r.has_value());
    return *r;
}

} // namespace

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

TEST_CASE("TT: probe on empty table returns miss", "[tt]") {
    TranspositionTable tt(1024);
    const auto probe = tt.probe(0xDEADBEEFBADC0FFEULL);
    REQUIRE_FALSE(probe.hit);
    REQUIRE(tt.probes() == 1);
    REQUIRE(tt.hits() == 0);
}

TEST_CASE("TT: probe returns what was stored", "[tt]") {
    TranspositionTable tt(1024);
    const ZobristKey k = 0xABC123ULL;
    Move m;
    m.from = Square::E2;
    m.to   = Square::E4;
    tt.store(k, /*depth=*/5, /*score=*/42, TtBound::Exact, m);

    const auto probe = tt.probe(k);
    REQUIRE(probe.hit);
    REQUIRE(probe.entry.depth == 5);
    REQUIRE(probe.entry.score == 42);
    REQUIRE(probe.entry.bound() == TtBound::Exact);
    REQUIRE(probe.entry.move == m);
}

TEST_CASE("TT: higher-depth store replaces lower-depth slot", "[tt]") {
    TranspositionTable tt(1024);
    const ZobristKey k = 0x111ULL;
    tt.store(k, 2, 10, TtBound::Exact, Move{});
    tt.store(k, 5, 20, TtBound::Exact, Move{});
    const auto probe = tt.probe(k);
    REQUIRE(probe.hit);
    REQUIRE(probe.entry.depth == 5);
    REQUIRE(probe.entry.score == 20);
}

TEST_CASE("TT: clear empties the table", "[tt]") {
    TranspositionTable tt(1024);
    tt.store(0x111ULL, 2, 10, TtBound::Exact, Move{});
    REQUIRE(tt.probe(0x111ULL).hit);
    tt.clear();
    REQUIRE_FALSE(tt.probe(0x111ULL).hit);
}

TEST_CASE("TT: size rounds down to a power of two", "[tt]") {
    TranspositionTable tt(1500);
    REQUIRE(tt.size() == 1024);
    TranspositionTable tt2(1024);
    REQUIRE(tt2.size() == 1024);
}

// ---------------------------------------------------------------------------
// Search-level integration
// ---------------------------------------------------------------------------

TEST_CASE("Search + TT: same best move, fewer nodes", "[tt][search]") {
    const std::string fen =
        "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 3";

    auto board_a = board_from(fen);
    const SearchResult without = Search::find_best(board_a, 4);

    auto board_b = board_from(fen);
    TranspositionTable tt(1u << 16);
    SearchLimits l; l.max_depth = 4;
    const SearchResult with = Search::find_best(board_b, l, &tt);

    // Alpha-beta with or without TT is a sound transformation —
    // the *score* must match. `best_move` can differ when
    // multiple moves share the same score: with move ordering
    // and TT hints in play, the search may pick a different
    // (but equally good) first move on ties.
    REQUIRE(with.score == without.score);

    // The TT run must save work. On iterative deepening, each
    // new depth re-enters many of the same nodes — the TT should
    // cut them off after a probe.
    REQUIRE(with.nodes < without.nodes);

    // Some probes must have been attempted and at least one must
    // have hit. (An empty statistic would mean the TT was never
    // consulted — a regression signal.)
    REQUIRE(with.tt_probes > 0);
    REQUIRE(with.tt_hits > 0);
}

TEST_CASE("Search + TT: still finds mate-in-2", "[tt][search][mate]") {
    auto b = board_from("k7/8/8/1QK5/8/8/8/8 w - - 0 1");
    TranspositionTable tt(1u << 16);
    SearchLimits l; l.max_depth = 4;
    const SearchResult r = Search::find_best(b, l, &tt);
    REQUIRE(Search::is_mate_score(r.score));
    REQUIRE(Search::plies_to_mate(r.score) == 3);
}

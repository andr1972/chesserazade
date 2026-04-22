/// Tests for the TreeRecorder hook on `Search::find_best`.
///
/// We attach a vector-backed recorder and check the recorded
/// event shape against known small positions — the numbers here
/// don't test alpha-beta correctness (other tests do that), they
/// test that the recorder sees the right events in the right
/// order with the right stats.
#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/search.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace chesserazade;

namespace {

struct Event {
    enum class Kind { Enter, Leave };
    Kind kind;
    int ply;
    Move move;           // only meaningful on Enter
    int score = 0;       // only meaningful on Leave
    bool was_cutoff = false;
    BranchStats stats;
};

class VectorRecorder final : public TreeRecorder {
public:
    explicit VectorRecorder(int cap) : cap_(cap) {}
    [[nodiscard]] int ply_cap() const noexcept override { return cap_; }

    void enter(int ply, const Move& m) override {
        events.push_back({Event::Kind::Enter, ply, m, 0, false, {}});
    }
    void leave(int ply, int score, bool was_cutoff,
               const BranchStats& s) override {
        events.push_back({Event::Kind::Leave, ply, Move{}, score,
                          was_cutoff, s});
    }

    std::vector<Event> events;

private:
    int cap_;
};

SearchLimits depth_only(int d) {
    SearchLimits l;
    l.max_depth = d;
    return l;
}

} // namespace

TEST_CASE("TreeRecorder: depth-1 visits every root move", "[recorder]") {
    auto b = *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    VectorRecorder rec(/*cap=*/1);
    (void)Search::find_best(b, depth_only(1), nullptr, &rec);

    // From the starting position there are 20 legal moves. Each
    // enter pairs with a leave at ply 1.
    std::size_t enters = 0;
    std::size_t leaves = 0;
    for (const auto& e : rec.events) {
        if (e.kind == Event::Kind::Enter) {
            REQUIRE(e.ply == 1);
            ++enters;
        } else {
            REQUIRE(e.ply == 1);
            ++leaves;
        }
    }
    REQUIRE(enters == 20);
    REQUIRE(leaves == 20);
}

TEST_CASE("TreeRecorder: events nest as enter/leave pairs",
          "[recorder]") {
    auto b = *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    VectorRecorder rec(/*cap=*/2);
    (void)Search::find_best(b, depth_only(2), nullptr, &rec);

    // Well-formed nesting: at any prefix of the event list, the
    // number of Enters >= number of Leaves; overall they must
    // balance. Also every Leave's ply must equal the matching
    // open Enter's ply.
    std::vector<int> stack;
    for (const auto& e : rec.events) {
        if (e.kind == Event::Kind::Enter) {
            stack.push_back(e.ply);
        } else {
            REQUIRE_FALSE(stack.empty());
            REQUIRE(stack.back() == e.ply);
            stack.pop_back();
        }
    }
    REQUIRE(stack.empty());
}

TEST_CASE("TreeRecorder: ply_cap=1 suppresses ply-2 events",
          "[recorder]") {
    auto b = *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    VectorRecorder rec(/*cap=*/1);
    (void)Search::find_best(b, depth_only(2), nullptr, &rec);

    for (const auto& e : rec.events) {
        REQUIRE(e.ply == 1);
    }
}

TEST_CASE("TreeRecorder: capture of a piece is tallied on the "
          "capturing side", "[recorder]") {
    // A position where white to move has one legal capture that
    // dominates: KQ vs K, white captures nothing at depth 1; we
    // craft a cleaner case. White rook takes black rook on an
    // otherwise empty board so the engine definitely picks it.
    constexpr std::string_view FEN = "4k3/8/8/8/8/8/4K3/R3r3 w - - 0 1";
    auto b = *Board8x8Mailbox::from_fen(FEN);

    VectorRecorder rec(/*cap=*/1);
    const auto r = Search::find_best(b, depth_only(1), nullptr, &rec);

    // PV must start with Rxe1 (the only sane move — rook takes
    // rook = +5). Its leave stat records a white capture of a
    // rook (500 cp).
    REQUIRE(to_uci(r.best_move) == "a1e1");
    REQUIRE(r.pv_stats.captures_white == 500);
    REQUIRE(r.pv_stats.captures_black == 0);

    // In the recorded events, the Leave for move "a1e1" carries
    // the same stat.
    bool saw = false;
    for (std::size_t i = 0; i + 1 < rec.events.size(); ++i) {
        if (rec.events[i].kind != Event::Kind::Enter) continue;
        if (to_uci(rec.events[i].move) != "a1e1") continue;
        const auto& lv = rec.events[i + 1];
        REQUIRE(lv.kind == Event::Kind::Leave);
        REQUIRE(lv.stats.captures_white == 500);
        REQUIRE(lv.stats.captures_black == 0);
        saw = true;
    }
    REQUIRE(saw);
}

TEST_CASE("TreeRecorder: a check-giving move increments the "
          "check counter for the side that gave it",
          "[recorder]") {
    // White to move; Rxd8+ takes the black queen and opens a
    // check down the d-file (white rook on d8, black king e8,
    // nothing between). No white piece sits on the d-file to
    // block, so this genuinely gives check.
    constexpr std::string_view FEN = "3qk3/8/8/8/8/8/8/3RK3 w - - 0 1";
    auto b = *Board8x8Mailbox::from_fen(FEN);

    VectorRecorder rec(/*cap=*/1);
    const auto r = Search::find_best(b, depth_only(1), nullptr, &rec);

    REQUIRE(to_uci(r.best_move) == "d1d8");
    REQUIRE(r.pv_stats.captures_white == 900); // queen
    REQUIRE(r.pv_stats.checks_white >= 1);
    REQUIRE(r.pv_stats.checks_black == 0);
}

TEST_CASE("TreeRecorder: cutoff flag fires on fail-high moves",
          "[recorder]") {
    // Any decent middlegame position with alpha-beta will see
    // cutoffs at depth 3+. We just assert that *at least one*
    // recorded Leave carries was_cutoff=true, confirming the flag
    // is wired and not always-false.
    auto b = *Board8x8Mailbox::from_fen(
        "r1bqkbnr/pppppppp/n7/8/8/N7/PPPPPPPP/R1BQKBNR w KQkq - 0 1");

    VectorRecorder rec(/*cap=*/3);
    (void)Search::find_best(b, depth_only(4), nullptr, &rec);

    bool saw_cutoff = false;
    for (const auto& e : rec.events) {
        if (e.kind == Event::Kind::Leave && e.was_cutoff) {
            saw_cutoff = true;
            break;
        }
    }
    REQUIRE(saw_cutoff);
}

TEST_CASE("TreeRecorder: nullptr recorder keeps legacy behaviour "
          "(no crash, same best move)", "[recorder]") {
    auto b = *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    const auto r1 = Search::find_best(b, depth_only(3), nullptr, nullptr);
    const auto r2 = Search::find_best(b, depth_only(3), nullptr);
    REQUIRE(r1.best_move == r2.best_move);
    REQUIRE(r1.score == r2.score);
}

TEST_CASE("TreeRecorder: pv_stats captures are populated even "
          "without a recorder", "[recorder]") {
    // The capture-value path is free (comes straight from
    // Move::captured_piece) so it runs unconditionally. The
    // rook-takes-rook position proves it.
    constexpr std::string_view FEN = "4k3/8/8/8/8/8/4K3/R3r3 w - - 0 1";
    auto b = *Board8x8Mailbox::from_fen(FEN);
    const auto r = Search::find_best(b, depth_only(1), nullptr, nullptr);
    REQUIRE(r.pv_stats.captures_white == 500);
    REQUIRE(r.pv_stats.captures_black == 0);
    // Check counters stay zero — no recorder means no per-node
    // check probe.
    REQUIRE(r.pv_stats.checks_white == 0);
    REQUIRE(r.pv_stats.checks_black == 0);
}

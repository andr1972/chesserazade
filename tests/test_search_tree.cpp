/// Tests for the analyzer's `SearchTreeRecorder` and
/// `SearchTree` — the data structures that back the 1.3.8 Qt
/// tree view, independent of Qt itself. We drive
/// `Search::find_best` with the recorder attached and verify
/// the resulting tree matches the enter / leave events one-
/// for-one. Regressions that show up as "everything flat under
/// the root" or "dangling open nodes after an abort" would be
/// caught here before reaching the UI.
#include "../analyzer/search_tree.hpp"

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/search.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace chesserazade;
using namespace chesserazade::analyzer;

namespace {

SearchLimits depth(int d) {
    SearchLimits l;
    l.max_depth = d;
    return l;
}

/// Walk the tree; return the depth of the deepest node from
/// the sentinel root (0 = no non-sentinel nodes).
int max_depth_of(const SearchTree& t, int idx = 0, int d = 0) {
    int best = d;
    for (int c : t.at(idx).children) {
        const int sub = max_depth_of(t, c, d + 1);
        if (sub > best) best = sub;
    }
    return best;
}

/// Verify every non-root node's parent pointer actually lists
/// it among its children. A flat "everything under root" bug
/// would fail this by making every node the parent's direct
/// child but the intermediate hops absent.
bool parent_links_consistent(const SearchTree& t) {
    for (int i = 1; i < t.size(); ++i) {
        const int p = t.at(i).parent;
        if (p < 0 || p >= t.size()) return false;
        const auto& kids = t.at(p).children;
        if (std::find(kids.begin(), kids.end(), i) == kids.end()) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("SearchTreeRecorder: depth 1 records a flat fringe",
          "[search_tree]") {
    auto b = *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    SearchTree tree;
    SearchTreeRecorder rec(tree, /*cap=*/3);

    (void)Search::find_best(b, depth(1), nullptr, &rec);

    // 20 starting-position legal moves; each is a direct child
    // of the sentinel with no grandchildren (horizon hit).
    REQUIRE(static_cast<int>(tree.at(0).children.size()) == 20);
    REQUIRE(max_depth_of(tree) == 1);
    REQUIRE(parent_links_consistent(tree));
}

TEST_CASE("SearchTreeRecorder: depth 2 produces a two-level tree",
          "[search_tree]") {
    auto b = *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    SearchTree tree;
    SearchTreeRecorder rec(tree, /*cap=*/3);

    (void)Search::find_best(b, depth(2), nullptr, &rec);

    // 20 root moves, and the first one (whichever it is after
    // move ordering) has its own ~20 replies. Some branches
    // may be TT-cut after the first iteration seeds the table
    // — but the first move searched is guaranteed full, since
    // nothing cuts before the first leaf.
    REQUIRE(tree.at(0).children.size() == 20);
    const int first_child = tree.at(0).children.front();
    REQUIRE_FALSE(tree.at(first_child).children.empty());
    REQUIRE(max_depth_of(tree) == 2);
    REQUIRE(parent_links_consistent(tree));
}

TEST_CASE("SearchTreeRecorder: cap caps the recorded depth even "
          "when search goes deeper", "[search_tree]") {
    auto b = *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    SearchTree tree;
    SearchTreeRecorder rec(tree, /*cap=*/2);

    (void)Search::find_best(b, depth(4), nullptr, &rec);

    // Recorder cap was 2 → the tree cannot be deeper than 2
    // regardless of search depth. Negamax still ran to depth
    // 4 (captures / checks in pv_stats would reflect that).
    REQUIRE(max_depth_of(tree) == 2);
    REQUIRE(parent_links_consistent(tree));
}

TEST_CASE("SearchTreeRecorder: reset clears to a bare sentinel",
          "[search_tree]") {
    auto b = *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    SearchTree tree;
    SearchTreeRecorder rec(tree, /*cap=*/2);

    (void)Search::find_best(b, depth(2), nullptr, &rec);
    REQUIRE(tree.at(0).children.size() > 0);

    rec.reset();
    REQUIRE(tree.size() == 1);   // sentinel only
    REQUIRE(tree.at(0).children.empty());
}

TEST_CASE("SearchTree::graft_under splices a sub-tree correctly",
          "[search_tree]") {
    // Synthesise a main tree with one parent node that still
    // has a search depth budget but no children recorded.
    SearchTree main;
    const int top = main.push_child(0, Move{}, 1);
    main.at(top).remaining_depth = 3;

    // Synthesise a sub tree with two plies of nesting.
    SearchTree sub;
    const int s_a = sub.push_child(0, Move{}, 1);
    const int s_b = sub.push_child(0, Move{}, 1);
    const int s_a1 = sub.push_child(s_a, Move{}, 2);
    const int s_a2 = sub.push_child(s_a, Move{}, 2);
    (void)s_b; (void)s_a1; (void)s_a2;

    const int main_size_before = main.size();
    main.graft_under(top, sub);

    // Two top-level children appended under `top` with
    // remapped indices.
    REQUIRE(main.at(top).children.size() == 2);
    const int grafted_a = main.at(top).children[0];
    const int grafted_b = main.at(top).children[1];
    REQUIRE(grafted_a >= main_size_before);
    REQUIRE(grafted_b >= main_size_before);

    // The grafted `a` should itself have two grafted children
    // — the nesting survived.
    REQUIRE(main.at(grafted_a).children.size() == 2);
    // Parent pointers are remapped.
    REQUIRE(main.at(grafted_a).parent == top);
    const int grafted_a1 = main.at(grafted_a).children[0];
    REQUIRE(main.at(grafted_a1).parent == grafted_a);
}

TEST_CASE("SearchTreeRecorder: lazy sub-search reproduces nesting",
          "[search_tree]") {
    // Main pass: cap=1 so only ply-1 moves are recorded.
    auto start = *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    SearchTree main;
    SearchTreeRecorder main_rec(main, /*cap=*/1);
    (void)Search::find_best(start, depth(4), nullptr, &main_rec);

    REQUIRE(main.at(0).children.size() == 20);
    // Each top-level move is a flat leaf in the main tree but
    // carries a positive remaining_depth (the main search went
    // 3 more plies below it).
    const int first = main.at(0).children.front();
    REQUIRE(main.at(first).children.empty());
    REQUIRE(main.at(first).remaining_depth > 0);

    // Rebuild the position at that node and run a sub-search
    // with the stored α-β window and remaining depth.
    Board8x8Mailbox b = start;
    b.make_move(main.at(first).move);

    SearchTree sub;
    SearchTreeRecorder sub_rec(sub, /*cap=*/2);
    SearchLimits lim;
    lim.max_depth = main.at(first).remaining_depth;
    (void)Search::find_best(b, lim, /*tt=*/nullptr, &sub_rec,
                            main.at(first).alpha,
                            main.at(first).beta);

    // The sub-tree itself must have nesting — that's the
    // "arrows after expansion" the UI depends on.
    REQUIRE_FALSE(sub.at(0).children.empty());
    REQUIRE(max_depth_of(sub) >= 2);

    // Graft + re-check: the previously-flat main leaf now has
    // a subtree under it.
    main.graft_under(first, sub);
    REQUIRE_FALSE(main.at(first).children.empty());
    REQUIRE(max_depth_of(main, first) >= 2);
    REQUIRE(parent_links_consistent(main));
}

TEST_CASE("SearchTreeRecorder: consecutive iterations isolate cleanly",
          "[search_tree]") {
    auto b = *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    SearchTree tree;
    SearchTreeRecorder rec(tree, /*cap=*/3);

    (void)Search::find_best(b, depth(1), nullptr, &rec);
    const int after_d1 = tree.size();

    // Second search from a fresh sentinel.
    rec.reset();
    (void)Search::find_best(b, depth(2), nullptr, &rec);

    // The d=2 tree must start from scratch — if the d=1 nodes
    // leaked through the d=2 recorder would overflow well
    // beyond what a single depth-2 pass can produce.
    REQUIRE(tree.at(0).children.size() == 20);
    REQUIRE(max_depth_of(tree) == 2);
    REQUIRE(parent_links_consistent(tree));
    (void)after_d1; // used for diagnosis if the REQUIREs fail.
}

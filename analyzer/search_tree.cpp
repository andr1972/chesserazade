#include "search_tree.hpp"

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/san.hpp>

namespace chesserazade::analyzer {

SearchTree::SearchTree() {
    reset();
}

void SearchTree::reset() {
    nodes_.clear();
    nodes_.push_back(TreeNode{});  // sentinel root, parent = -1.
}

int SearchTree::push_child(int parent, const Move& move, int ply) {
    TreeNode n;
    n.move = move;
    n.parent = parent;
    // `ply` is kept implicitly via the parent chain; no separate
    // field on TreeNode to stay 1.3.0's ~20-30 B/node target.
    (void)ply;
    const int idx = static_cast<int>(nodes_.size());
    nodes_.push_back(std::move(n));
    nodes_[static_cast<std::size_t>(parent)].children.push_back(idx);
    return idx;
}

namespace {

/// Depth-first walk that fills in the `san` field on every
/// non-root node. Uses an explicit working board that is
/// incrementally advanced and rolled back — avoids copying
/// the board at every recursive step.
void san_dfs(SearchTree& t, int idx, Board8x8Mailbox& b) {
    const TreeNode& here = t.at(idx);
    for (int c : here.children) {
        const Move m = t.at(c).move;
        t.at(c).san = to_san(b, m);
        b.make_move(m);
        san_dfs(t, c, b);
        b.unmake_move(m);
    }
}

} // namespace

void SearchTree::finalize_san(const Board8x8Mailbox& start) {
    Board8x8Mailbox work = start;
    san_dfs(*this, 0, work);
}

// ---------------------------------------------------------------------------
// SearchTreeRecorder
// ---------------------------------------------------------------------------

SearchTreeRecorder::SearchTreeRecorder(SearchTree& tree, int cap)
    : tree_(tree), cap_(cap) {
    reset();
}

void SearchTreeRecorder::reset() {
    tree_.reset();
    stack_.clear();
    stack_.push_back(0); // sentinel root is always open.
}

void SearchTreeRecorder::begin_iteration(int /*depth*/) {
    // Each iteration of iterative deepening re-searches the
    // whole tree from the root. Without resetting here, the
    // root would accumulate N × top-level-moves entries after
    // `find_best(max_depth=N)` — 40 siblings at depth 2, 60 at
    // depth 3, and so on.
    reset();
}

void SearchTreeRecorder::enter(int /*ply*/, const Move& move) {
    const int parent = stack_.back();
    const int idx = tree_.push_child(parent, move, /*ply=*/0);
    stack_.push_back(idx);
}

void SearchTreeRecorder::leave(int /*ply*/, int score, bool was_cutoff,
                               const BranchStats& stats) {
    const int idx = stack_.back();
    stack_.pop_back();
    TreeNode& n = tree_.at(idx);
    n.score = score;
    n.was_cutoff = was_cutoff;
    n.stats = stats;
}

} // namespace chesserazade::analyzer

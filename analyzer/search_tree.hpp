/// Flat arena that collects the tree of moves explored by
/// `Search::find_best` — one entry per `TreeRecorder::enter` /
/// `leave` pair. The tree is capped at `ply_cap` plies; deeper
/// search still runs, but those nodes are invisible here and
/// their stats bubble up as aggregates on the PV branch (see
/// `BranchStats`).
///
/// Nodes form a parent/child forest rooted at index 0 (a
/// sentinel with no move). Storing indices instead of pointers
/// keeps the model simple and cache-friendly; `QModelIndex`
/// just stashes the node index in its `internalId`.
#pragma once

#include <chesserazade/move.hpp>
#include <chesserazade/search.hpp>

#include <string>
#include <vector>

namespace chesserazade { class Board8x8Mailbox; }

namespace chesserazade::analyzer {

struct TreeNode {
    /// The move that leads into this node (from the parent's
    /// position). Undefined for the sentinel root.
    Move move{};

    /// Short-algebraic rendering — filled in by
    /// `SearchTree::finalize_san` after the search.
    std::string san;

    /// Score reported by negamax at this node, from the side-
    /// to-move's perspective (centipawns, or a mate score).
    int score = 0;

    /// True when this move caused a beta cutoff at its parent
    /// — the "tu nastąpiło cięcie" marker for the tree view.
    bool was_cutoff = false;

    /// True when this node sits on the principal variation of
    /// the final completed iteration. Populated after the
    /// search by `SearchTree::mark_pv`; renders bold in the
    /// tree view.
    bool on_pv = false;

    /// Captures and checks summed along the principal
    /// variation in this subtree (this move inclusive).
    BranchStats stats{};

    int parent = -1;                 // -1 only for the sentinel root.
    std::vector<int> children;       // indices into `SearchTree::nodes`.
};

class SearchTree {
public:
    SearchTree();

    /// Clear the arena and install a fresh sentinel root.
    void reset();

    [[nodiscard]] int size() const noexcept
        { return static_cast<int>(nodes_.size()); }
    [[nodiscard]] const TreeNode& at(int i) const { return nodes_[static_cast<std::size_t>(i)]; }
    [[nodiscard]] TreeNode& at(int i)             { return nodes_[static_cast<std::size_t>(i)]; }

    /// Allocate a new node with the given `move` whose parent
    /// is `parent`. Returns the new node's index.
    int push_child(int parent, const Move& move, int ply);

    [[nodiscard]] const std::vector<TreeNode>& nodes() const noexcept
        { return nodes_; }

    /// Compute SAN strings on every non-root node by walking
    /// the tree with a working board starting at `start`.
    void finalize_san(const chesserazade::Board8x8Mailbox& start);

    /// Mark the highest-scoring child under every internal
    /// node (including the sentinel). That recovers the PV
    /// from the root and — just as importantly — the locally
    /// best reply at every subtree, so when the user expands
    /// a non-PV node the best continuation in that line is
    /// still bolded.
    void mark_best_subtrees();

private:
    std::vector<TreeNode> nodes_;
};

/// Concrete `TreeRecorder` that writes into a `SearchTree`.
/// Nodes at ply > `cap` are silently dropped at the source
/// (we just refuse to record them in the stack).
class SearchTreeRecorder final : public TreeRecorder {
public:
    SearchTreeRecorder(SearchTree& tree, int cap);

    [[nodiscard]] int ply_cap() const noexcept override { return cap_; }
    void begin_iteration(int depth) override;
    void enter(int ply, const Move& move) override;
    void leave(int ply, int score, bool was_cutoff,
               const BranchStats& stats) override;

    /// Re-anchor to a fresh tree state (clears the node stack
    /// back to the sentinel root). Normally invoked from
    /// `begin_iteration`; tests may call it directly.
    void reset();

private:
    SearchTree& tree_;
    int cap_;
    std::vector<int> stack_;  // open-node indices; stack_[0] == 0 (root).
};

} // namespace chesserazade::analyzer

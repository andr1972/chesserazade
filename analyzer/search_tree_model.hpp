/// `SearchTreeModel` — adapts a `SearchTree` to QTreeView.
///
/// Columns, left to right:
///   0  Move (SAN) — also carries the cutoff icon
///   1  Score (cp or "mate N")
///   2  Captures (white)
///   3  Captures (black)
///   4  Checks (white)
///   5  Checks (black)
///
/// `QModelIndex::internalId` stores the tree's node index. The
/// sentinel root (node 0) is always the invisible root of the
/// Qt model; its children become the top-level rows the user
/// sees. Cutoff rows render with a red foreground.
#pragma once

#include "search_tree.hpp"

#include <QAbstractItemModel>

namespace chesserazade::analyzer {

class SearchTreeModel final : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column : int {
        ColId = 0,  // 1-based sibling index under the parent.
        ColMove,
        ColScore,
        ColCapW,
        ColCapB,
        ColChkW,
        ColChkB,
        ColNodes,   // total engine visits in this subtree.
        ColumnCount,
    };

    explicit SearchTreeModel(QObject* parent = nullptr);

    /// Re-anchor to a new tree (non-owning — the tree must
    /// outlive the model). Pass `nullptr` to clear. The
    /// pointer is non-const so the model can graft sub-
    /// searches into the tree in-place on lazy expansion.
    void set_tree(SearchTree* tree);

    /// Toggle the lazy-expansion machinery. When off,
    /// `canFetchMore` stays false for every node, so Qt never
    /// asks to fetch children and clicking an arrow becomes a
    /// no-op; `hasChildren` still reports true for cap-leaves
    /// with a positive `remaining_depth` so the arrows remain
    /// visible for inspection. On by default.
    void set_lazy_enabled(bool enabled) { lazy_enabled_ = enabled; }
    [[nodiscard]] bool lazy_enabled() const noexcept
        { return lazy_enabled_; }

    /// Graft `sub`'s children under the tree node at index
    /// `parent_node` and notify the view. Used by the panel
    /// after a successful on-demand sub-search.
    void insert_subtree(int parent_node, const SearchTree& sub);

    /// Map an underlying tree-node index back to a column-0
    /// QModelIndex. Useful to scroll to / re-expand a node
    /// after the tree has been grafted.
    [[nodiscard]] QModelIndex index_for_node(int node_idx) const;

    [[nodiscard]] bool hasChildren(const QModelIndex& parent) const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex& parent) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent) const override;
    [[nodiscard]] QVariant data(const QModelIndex& idx, int role) const override;
    [[nodiscard]] QVariant headerData(int section,
                                      Qt::Orientation orientation,
                                      int role) const override;

    /// Return the chain of `Move`s from the sentinel root down
    /// to the node referenced by `idx` — used by the panel to
    /// replay moves onto the board when the user clicks a row.
    [[nodiscard]] std::vector<Move>
    moves_to(const QModelIndex& idx) const;

signals:
    /// A cap-bounded node was expanded by the user and has no
    /// children yet. The panel runs a sub-search from that
    /// node's position and calls `insert_subtree`.
    void expansion_requested(int node_idx);

private:
    [[nodiscard]] int node_of(const QModelIndex& idx) const noexcept;

    SearchTree* tree_ = nullptr;
    /// Nodes whose expansion signal is queued but not yet
    /// handled. Suppresses duplicate requests when the view
    /// re-asks canFetchMore between emit and graft.
    mutable std::vector<int> pending_expansions_;

    bool lazy_enabled_ = true;
};

} // namespace chesserazade::analyzer

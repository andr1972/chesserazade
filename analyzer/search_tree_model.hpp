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
        ColMove = 0,
        ColScore,
        ColCapW,
        ColCapB,
        ColChkW,
        ColChkB,
        ColumnCount,
    };

    explicit SearchTreeModel(QObject* parent = nullptr);

    /// Re-anchor to a new tree (non-owning — the tree must
    /// outlive the model). Pass `nullptr` to clear.
    void set_tree(const SearchTree* tree);

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

private:
    [[nodiscard]] int node_of(const QModelIndex& idx) const noexcept;

    const SearchTree* tree_ = nullptr;
};

} // namespace chesserazade::analyzer

#include "search_tree_model.hpp"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QString>

namespace chesserazade::analyzer {

SearchTreeModel::SearchTreeModel(QObject* parent)
    : QAbstractItemModel(parent) {}

void SearchTreeModel::set_tree(const SearchTree* tree) {
    beginResetModel();
    tree_ = tree;
    endResetModel();
}

int SearchTreeModel::node_of(const QModelIndex& idx) const noexcept {
    // Invalid Qt index → Qt's hidden root. Its single child is
    // the tree sentinel (tree node 0). A valid Qt index stores
    // the tree node directly in `internalId`.
    if (!idx.isValid()) return -1;
    return static_cast<int>(idx.internalId());
}

QModelIndex SearchTreeModel::index(int row, int column,
                                   const QModelIndex& parent) const {
    if (tree_ == nullptr) return {};
    if (column < 0 || column >= ColumnCount) return {};

    // The Qt-invisible root has exactly one child: the tree's
    // sentinel (node 0), shown as the "/" row.
    if (!parent.isValid()) {
        if (row != 0) return {};
        return createIndex(row, column, static_cast<quintptr>(0));
    }

    const int pn = node_of(parent);
    if (pn < 0 || pn >= tree_->size()) return {};
    const auto& kids = tree_->at(pn).children;
    if (row < 0 || row >= static_cast<int>(kids.size())) return {};
    const int child = kids[static_cast<std::size_t>(row)];
    return createIndex(row, column, static_cast<quintptr>(child));
}

QModelIndex SearchTreeModel::parent(const QModelIndex& child) const {
    if (tree_ == nullptr || !child.isValid()) return {};
    const int n = node_of(child);
    if (n <= 0) return {};                     // sentinel or invalid → Qt root
    const int p = tree_->at(n).parent;
    if (p < 0) return {};
    if (p == 0) {
        // Parent is the sentinel — the single top-level row.
        return createIndex(0, 0, static_cast<quintptr>(0));
    }
    const int gp = tree_->at(p).parent;
    if (gp < 0) return {};
    const auto& sibs = tree_->at(gp).children;
    for (std::size_t i = 0; i < sibs.size(); ++i) {
        if (sibs[i] == p) {
            return createIndex(static_cast<int>(i), 0,
                               static_cast<quintptr>(p));
        }
    }
    return {};
}

int SearchTreeModel::rowCount(const QModelIndex& parent) const {
    if (tree_ == nullptr) return 0;
    if (!parent.isValid()) return 1;           // just the sentinel row
    const int n = node_of(parent);
    if (n < 0 || n >= tree_->size()) return 0;
    return static_cast<int>(tree_->at(n).children.size());
}

int SearchTreeModel::columnCount(const QModelIndex& /*parent*/) const {
    return ColumnCount;
}

namespace {

[[nodiscard]] QString format_score(int score) {
    if (Search::is_mate_score(score)) {
        return QStringLiteral("mate %1").arg(Search::plies_to_mate(score));
    }
    return QStringLiteral("%1").arg(score);
}

} // namespace

QVariant SearchTreeModel::data(const QModelIndex& idx, int role) const {
    if (tree_ == nullptr || !idx.isValid()) return {};
    const int n = node_of(idx);
    if (n < 0 || n >= tree_->size()) return {};

    // The sentinel row represents the starting position of the
    // solve — clicking it resets the board to the pre-solve
    // snapshot. It has no move, no score, no stats.
    if (n == 0) {
        if (role != Qt::DisplayRole) return {};
        if (idx.column() == ColMove) return QStringLiteral("/");
        return {};
    }

    const TreeNode& node = tree_->at(n);

    if (role == Qt::ForegroundRole && node.was_cutoff) {
        return QBrush(QColor(0xb0, 0x28, 0x28));
    }
    if (role == Qt::FontRole && node.on_pv) {
        QFont f;
        f.setBold(true);
        return f;
    }
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};

    switch (idx.column()) {
        case ColId:   return idx.row() + 1;
        case ColMove: {
            const QString base = node.san.empty()
                ? QString::fromStdString(to_uci(node.move))
                : QString::fromStdString(node.san);
            if (role == Qt::ToolTipRole && node.was_cutoff) {
                return QStringLiteral("%1 — caused β-cutoff").arg(base);
            }
            return node.was_cutoff
                ? QStringLiteral("%1  [cut]").arg(base)
                : base;
        }
        case ColScore: return format_score(node.score);
        case ColCapW:  return node.stats.captures_white;
        case ColCapB:  return node.stats.captures_black;
        case ColChkW:  return node.stats.checks_white;
        case ColChkB:  return node.stats.checks_black;
        default:       return {};
    }
}

QVariant SearchTreeModel::headerData(int section,
                                     Qt::Orientation orientation,
                                     int role) const {
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Vertical) return section + 1;
    switch (section) {
        case ColId:    return tr("#");
        case ColMove:  return tr("Move");
        case ColScore: return tr("Score");
        case ColCapW:  return tr("Capt W");
        case ColCapB:  return tr("Capt B");
        case ColChkW:  return tr("Chk W");
        case ColChkB:  return tr("Chk B");
        default:       return {};
    }
}

std::vector<Move> SearchTreeModel::moves_to(const QModelIndex& idx) const {
    std::vector<Move> out;
    if (tree_ == nullptr || !idx.isValid()) return out;
    int n = node_of(idx);
    while (n > 0) {
        out.push_back(tree_->at(n).move);
        n = tree_->at(n).parent;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace chesserazade::analyzer

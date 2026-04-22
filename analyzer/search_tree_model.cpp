#include "search_tree_model.hpp"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QString>

namespace chesserazade::analyzer {

SearchTreeModel::SearchTreeModel(QObject* parent)
    : QAbstractItemModel(parent) {}

void SearchTreeModel::set_tree(SearchTree* tree) {
    beginResetModel();
    tree_ = tree;
    recompute_filter();
    endResetModel();
}

void SearchTreeModel::set_filter(const FilterState& f) {
    beginResetModel();
    filter_ = f;
    recompute_filter();
    endResetModel();
}

namespace {

[[nodiscard]] bool is_capture_move(const Move& m) noexcept {
    return m.kind == MoveKind::Capture
        || m.kind == MoveKind::PromotionCapture
        || m.kind == MoveKind::EnPassant;
}

} // namespace

void SearchTreeModel::recompute_filter() {
    visible_children_.clear();
    in_tree_count_.clear();
    if (tree_ == nullptr) return;

    const int n = tree_->size();
    visible_children_.assign(static_cast<std::size_t>(n), {});
    in_tree_count_.assign(static_cast<std::size_t>(n), 0);

    if (!filter_.active()) {
        // Fast path: mirror the tree exactly.
        for (int i = 0; i < n; ++i) {
            visible_children_[static_cast<std::size_t>(i)] =
                tree_->at(i).children;
        }
        // Post-order descendant count.
        for (int i = n - 1; i >= 0; --i) {
            int c = 1;
            for (int ch : tree_->at(i).children) {
                c += in_tree_count_[static_cast<std::size_t>(ch)];
            }
            in_tree_count_[static_cast<std::size_t>(i)] = c;
        }
        return;
    }

    // Compute each node's depth (= ply from sentinel). Then a
    // node "matches" iff, for any ply in captures_on_ply, the
    // ancestor at that ply is a capture move; or analogously
    // for checks. The sentinel never matches (it has no move).
    std::vector<int> depth(static_cast<std::size_t>(n), 0);
    for (int i = 1; i < n; ++i) {
        depth[static_cast<std::size_t>(i)] =
            depth[static_cast<std::size_t>(tree_->at(i).parent)] + 1;
    }

    // For each non-sentinel node collect the ancestor chain
    // and test the filter.
    std::vector<bool> match(static_cast<std::size_t>(n), false);
    for (int i = 1; i < n; ++i) {
        // Walk up to root gathering per-ply capture/check flags.
        const int d = depth[static_cast<std::size_t>(i)];
        std::vector<bool> cap_at(static_cast<std::size_t>(d) + 1, false);
        std::vector<bool> chk_at(static_cast<std::size_t>(d) + 1, false);
        for (int cur = i; cur > 0;
             cur = tree_->at(cur).parent) {
            const int dep = depth[static_cast<std::size_t>(cur)];
            cap_at[static_cast<std::size_t>(dep)] =
                is_capture_move(tree_->at(cur).move);
            chk_at[static_cast<std::size_t>(dep)] =
                tree_->at(cur).gives_check;
        }

        bool any = false;
        for (std::size_t p = 0;
             p < filter_.captures_on_ply.size()
             && p + 1 < cap_at.size();
             ++p) {
            if (filter_.captures_on_ply[p] && cap_at[p + 1]) {
                any = true; break;
            }
        }
        if (!any) {
            for (std::size_t p = 0;
                 p < filter_.checks_on_ply.size()
                 && p + 1 < chk_at.size();
                 ++p) {
                if (filter_.checks_on_ply[p] && chk_at[p + 1]) {
                    any = true; break;
                }
            }
        }
        match[static_cast<std::size_t>(i)] = any;
    }

    // Propagate: a node is visible if it itself matches or
    // any descendant does. Bottom-up pass (indices in
    // insertion order guarantee parents precede their late
    // descendants, but for safety we do two passes).
    std::vector<bool> visible(static_cast<std::size_t>(n), false);
    for (int i = n - 1; i >= 1; --i) {
        if (match[static_cast<std::size_t>(i)]) {
            visible[static_cast<std::size_t>(i)] = true;
        }
        if (visible[static_cast<std::size_t>(i)]) {
            const int p = tree_->at(i).parent;
            if (p > 0) visible[static_cast<std::size_t>(p)] = true;
        }
    }
    // Sentinel is always visible.
    visible[0] = true;

    // Build visible_children_ in original order.
    for (int i = 0; i < n; ++i) {
        for (int ch : tree_->at(i).children) {
            if (visible[static_cast<std::size_t>(ch)]) {
                visible_children_[static_cast<std::size_t>(i)]
                    .push_back(ch);
            }
        }
    }

    // Descendant count bottom-up.
    for (int i = n - 1; i >= 0; --i) {
        int c = visible[static_cast<std::size_t>(i)] ? 1 : 0;
        for (int ch : visible_children_[static_cast<std::size_t>(i)]) {
            c += in_tree_count_[static_cast<std::size_t>(ch)];
        }
        in_tree_count_[static_cast<std::size_t>(i)] = c;
    }
}

const std::vector<int>& SearchTreeModel::visible_kids(int n) const {
    static const std::vector<int> empty;
    if (n < 0
        || static_cast<std::size_t>(n) >= visible_children_.size()) {
        return empty;
    }
    return visible_children_[static_cast<std::size_t>(n)];
}

QModelIndex SearchTreeModel::index_for_node(int node_idx) const {
    if (tree_ == nullptr) return {};
    if (node_idx == 0) return createIndex(0, 0, static_cast<quintptr>(0));
    if (node_idx < 0 || node_idx >= tree_->size()) return {};
    const int p = tree_->at(node_idx).parent;
    if (p < 0) return {};
    const auto& sibs = visible_kids(p);
    for (std::size_t i = 0; i < sibs.size(); ++i) {
        if (sibs[i] == node_idx) {
            return createIndex(static_cast<int>(i), 0,
                               static_cast<quintptr>(node_idx));
        }
    }
    return {};
}

bool SearchTreeModel::hasChildren(const QModelIndex& parent) const {
    if (tree_ == nullptr) return false;
    if (!parent.isValid()) return true;     // invisible root → sentinel
    const int n = node_of(parent);
    if (n < 0 || n >= tree_->size()) return false;
    if (!visible_kids(n).empty()) return true;
    // Sentinel always shows children; cap-bounded leaves get
    // an arrow if the main search could have gone deeper —
    // the user expands, the panel runs a mini-search, the
    // children appear.
    if (n == 0) return true;
    // When a filter is active we deliberately hide arrows on
    // cap-leaves whose visible subtree is empty — the user
    // asked to see only matching branches, not potential ones.
    if (filter_.active()) return false;
    return tree_->at(n).remaining_depth > 0;
}

bool SearchTreeModel::canFetchMore(const QModelIndex& parent) const {
    if (!lazy_enabled_) return false;
    if (tree_ == nullptr || !parent.isValid()) return false;
    const int n = node_of(parent);
    if (n <= 0 || n >= tree_->size()) return false;
    const TreeNode& node = tree_->at(n);
    if (!node.children.empty()) return false;
    if (node.remaining_depth <= 0) return false;
    // A queued expansion is already in flight; don't ask the
    // view to keep firing fetchMore while we wait for it.
    for (int p : pending_expansions_) {
        if (p == n) return false;
    }
    return true;
}

void SearchTreeModel::fetchMore(const QModelIndex& parent) {
    if (tree_ == nullptr || !parent.isValid()) return;
    const int n = node_of(parent);
    if (n <= 0 || n >= tree_->size()) return;
    for (int p : pending_expansions_) {
        if (p == n) return;
    }
    pending_expansions_.push_back(n);
    emit expansion_requested(n);
}

void SearchTreeModel::insert_subtree(int parent_node, const SearchTree& sub) {
    if (tree_ == nullptr || parent_node < 0
        || parent_node >= tree_->size()) return;
    // Drop this node from the pending-set whatever the
    // outcome — the caller has handled the request.
    for (std::size_t i = 0; i < pending_expansions_.size(); ++i) {
        if (pending_expansions_[i] == parent_node) {
            pending_expansions_.erase(pending_expansions_.begin()
                                      + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    const int add = static_cast<int>(sub.at(0).children.size());
    if (add == 0) return;
    // Brute-force a full model reset. beginInsertRows under a
    // not-yet-expanded parent turned out to leave Qt with a
    // stale row-count cache — new rows rendered at the wrong
    // nesting depth (as siblings of the parent rather than
    // its children). Grafting inside a reset is heavier but
    // makes the view re-query every index from scratch.
    beginResetModel();
    tree_->graft_under(parent_node, sub);
    recompute_filter();
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
    const auto& kids = visible_kids(pn);
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
    const auto& sibs = visible_kids(gp);
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
    return static_cast<int>(visible_kids(n).size());
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
    // snapshot. It has no move, no score, no per-move stats,
    // but we DO aggregate total engine work and in-tree count
    // over all its (visible) children so the user sees the
    // whole search's effort and the filter's effective size at
    // a glance on the "/" row.
    if (n == 0) {
        if (role != Qt::DisplayRole) return {};
        if (idx.column() == ColMove) return QStringLiteral("/");
        if (idx.column() == ColNodes) {
            std::uint64_t sum = 0;
            for (int c : tree_->at(0).children) {
                sum += tree_->at(c).subtree_nodes;
            }
            return QVariant::fromValue(sum);
        }
        if (idx.column() == ColInTree
            && !in_tree_count_.empty()) {
            return in_tree_count_[0] - 1; // exclude the sentinel itself
        }
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
        case ColNodes: return QVariant::fromValue(node.subtree_nodes);
        case ColInTree: {
            const int ni = n;
            if (ni < static_cast<int>(in_tree_count_.size())) {
                return in_tree_count_[static_cast<std::size_t>(ni)];
            }
            return {};
        }
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
        case ColNodes: return tr("Nodes");
        case ColInTree: return tr("InTree");
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

#include "game_list_model.hpp"

#include <QString>

namespace chesserazade::analyzer {

GameListModel::GameListModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void GameListModel::set_games(const std::vector<PgnGameHeader>* games) {
    beginResetModel();
    games_ = games;
    endResetModel();
}

int GameListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return games_ == nullptr
               ? 0
               : static_cast<int>(games_->size());
}

int GameListModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ColumnCount;
}

QVariant GameListModel::data(const QModelIndex& idx, int role) const {
    if (games_ == nullptr) return {};
    if (!idx.isValid()) return {};
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};

    const auto row = static_cast<std::size_t>(idx.row());
    if (row >= games_->size()) return {};
    const PgnGameHeader& g = (*games_)[row];

    switch (idx.column()) {
        case ColDate:   return QString::fromStdString(g.date);
        case ColWhite:  return QString::fromStdString(g.white);
        case ColBlack:  return QString::fromStdString(g.black);
        case ColResult: return QString::fromStdString(g.result);
        case ColEvent:  return QString::fromStdString(g.event);
        default:        return {};
    }
}

QVariant GameListModel::headerData(int section,
                                   Qt::Orientation orientation,
                                   int role) const {
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Vertical) return section + 1;
    switch (section) {
        case ColDate:   return tr("Date");
        case ColWhite:  return tr("White");
        case ColBlack:  return tr("Black");
        case ColResult: return tr("Result");
        case ColEvent:  return tr("Event");
        default:        return {};
    }
}

} // namespace chesserazade::analyzer

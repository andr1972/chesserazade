// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include "game_list_model.hpp"

#include <QString>

namespace chesserazade::analyzer {

GameListModel::GameListModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void GameListModel::set_games(const std::vector<GameRecord>* games) {
    beginResetModel();
    games_ = games;
    endResetModel();
}

const GameRecord* GameListModel::record_at(int row) const noexcept {
    if (games_ == nullptr) return nullptr;
    if (row < 0) return nullptr;
    const auto r = static_cast<std::size_t>(row);
    if (r >= games_->size()) return nullptr;
    return &(*games_)[r];
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
    const GameRecord& rec = (*games_)[row];
    const PgnGameHeader& g = rec.header;

    switch (idx.column()) {
        case ColDate:   return QString::fromStdString(g.date);
        case ColWhite:  return QString::fromStdString(g.white);
        case ColBlack:  return QString::fromStdString(g.black);
        case ColResult: return QString::fromStdString(g.result);
        case ColEco:    return QString::fromStdString(g.eco);
        case ColPlies:  return g.ply_count;
        case ColEnd:
            switch (rec.end_kind) {
                case EndKind::Mate:      return QStringLiteral("#");
                case EndKind::Stalemate: return QStringLiteral("=");
                case EndKind::Other:     return QString{};
                case EndKind::Unknown:   return QString{};
            }
            return QString{};
        case ColUP: {
            QString s;
            for (const auto& up : rec.underpromotions) {
                switch (up.piece) {
                    case PieceType::Knight: s += QLatin1Char('N'); break;
                    case PieceType::Bishop: s += QLatin1Char('B'); break;
                    case PieceType::Rook:   s += QLatin1Char('R'); break;
                    default: break;
                }
            }
            return s;
        }
        case ColKF:
            if (rec.knight_fork_plies.empty()) return QString{};
            return static_cast<int>(rec.knight_fork_plies.size());
        case ColSac: {
            // Summarize by the biggest single sacrifice's net
            // loss and the fraction recovered. E.g. "Q 60%"
            // means "queen-value net loss, 60% won back within
            // the window". No sacrifice → blank.
            if (rec.material_sacs.empty()) return QString{};
            const MaterialSac* best = &rec.material_sacs.front();
            for (const auto& s : rec.material_sacs) {
                if (s.loss_cp > best->loss_cp) best = &s;
            }
            const char* tag = "?";
            if      (best->loss_cp >= 900) tag = "Q";
            else if (best->loss_cp >= 500) tag = "R";
            else if (best->loss_cp >= 300) tag = "m";
            const int pct = (best->loss_cp > 0)
                ? (100 * best->recovery_cp / best->loss_cp)
                : 0;
            return QStringLiteral("%1 %2%").arg(tag).arg(pct);
        }
        case ColRaw: {
            // Largest single-piece drop regardless of recapture.
            // Useful when the "net" view hides the true piece
            // sacrificed — Fischer's 17…Be6!! / 18.Bxb6 nets
            // to rook-value but the queen physically fell.
            if (rec.material_sacs.empty()) return QString{};
            int raw = 0;
            for (const auto& s : rec.material_sacs) {
                if (s.raw_loss_cp > raw) raw = s.raw_loss_cp;
            }
            if      (raw >= 900) return QStringLiteral("Q");
            else if (raw >= 500) return QStringLiteral("R");
            else if (raw >= 300) return QStringLiteral("m");
            return QString{};
        }
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
        case ColEco:    return tr("ECO");
        case ColPlies:  return tr("Plies");
        case ColEnd:    return tr("End");
        case ColUP:     return tr("UP");
        case ColKF:     return tr("KF");
        case ColSac:    return tr("Sac");
        case ColRaw:    return tr("Raw");
        case ColEvent:  return tr("Event");
        default:        return {};
    }
}

} // namespace chesserazade::analyzer

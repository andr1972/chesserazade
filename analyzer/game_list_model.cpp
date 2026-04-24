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
            // Picks the sacrifice with the biggest *raw* single-
            // piece drop and shows that piece's letter directly
            // from its PieceType, plus how much of its value was
            // won back within the 20-ply forward window (net,
            // not peak — further losses subtract). "Q 100%" = a
            // queen physically fell and an equivalent queen's
            // worth was recovered by window end.
            if (rec.material_sacs.empty()) return QString{};
            const MaterialSac* best = &rec.material_sacs.front();
            for (const auto& s : rec.material_sacs) {
                if (s.raw_loss_cp > best->raw_loss_cp) best = &s;
            }
            QChar letter;
            switch (best->raw_piece) {
                case PieceType::Queen:  letter = QLatin1Char('Q'); break;
                case PieceType::Rook:   letter = QLatin1Char('R'); break;
                case PieceType::Bishop: letter = QLatin1Char('B'); break;
                case PieceType::Knight: letter = QLatin1Char('N'); break;
                case PieceType::Pawn:   letter = QLatin1Char('P'); break;
                default:                letter = QLatin1Char('?'); break;
            }
            const int pct = (best->raw_loss_cp > 0)
                ? (100 * best->recovery_cp / best->raw_loss_cp)
                : 0;
            return QStringLiteral("%1 %2%").arg(letter).arg(pct);
        }
        case ColEvent:  return QString::fromStdString(g.event);
        default:        return {};
    }
}

QVariant GameListModel::headerData(int section,
                                   Qt::Orientation orientation,
                                   int role) const {
    if (orientation == Qt::Vertical) {
        return (role == Qt::DisplayRole)
            ? QVariant{section + 1}
            : QVariant{};
    }
    if (role == Qt::DisplayRole) {
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
            case ColEvent:  return tr("Event");
            default:        return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (section) {
            case ColDate:   return tr("Date from the PGN tag, "
                                      "YYYY.MM.DD (parts may be "
                                      "question-marks).");
            case ColWhite:  return tr("Player of the white pieces.");
            case ColBlack:  return tr("Player of the black pieces.");
            case ColResult: return tr("Game result from the PGN tag:\n"
                                      "  1-0 — white won\n"
                                      "  0-1 — black won\n"
                                      "  1/2-1/2 — draw\n"
                                      "  * — unfinished or unknown");
            case ColEco:    return tr("ECO opening code (e.g. B20, C42). "
                                      "Blank when the PGN does not carry it.");
            case ColPlies:  return tr("Half-move count of the main line. "
                                      "Variations and comments are excluded.");
            case ColEnd:    return tr(
                "Detected board state at the final move:\n"
                "  # — checkmate (no legal reply, side to move in check)\n"
                "  = — stalemate (no legal reply, not in check)\n"
                "  blank — game ended off the board (resignation, "
                "timeout, draw agreement, adjudication)\n\n"
                "Click a row in this column to jump to the last move.");
            case ColUP:     return tr(
                "Under-promotions (non-queen). Piece letters in order "
                "of occurrence: N = knight, B = bishop, R = rook. "
                "Rare in master games; classic motifs include knight "
                "promotion for a fork or bishop/rook to dodge "
                "stalemate.\n\n"
                "Click this column to jump to the first under-"
                "promotion.");
            case ColKF:     return tr(
                "Count of knight forks — knight moves (or promotions "
                "to knight) that give check AND simultaneously attack "
                "an opposing queen or rook. Classic \"family check\" / "
                "royal fork motif.\n\n"
                "Click this column to jump to the first knight fork.");
            case ColSac:    return tr(
                "Biggest piece sacrificed, with recovery % over a "
                "20-ply forward window.\n\n"
                "Letter = piece that physically dropped "
                "(P/N/B/R/Q).\n"
                "% = how much of that piece's cp value was recovered "
                "by the end of the 20-ply window (endpoint minus "
                "settle point, so later losses subtract).\n\n"
                "Detection: the 2-ply exchange window is extended "
                "through any chain of consecutive captures (up to "
                "6 plies) so a multi-capture tower nets correctly. "
                "Trigger threshold: 300 cp net loss. Plain trades "
                "(R-for-R, even-value swaps) net to zero and are "
                "skipped.\n\n"
                "Click this column to jump to the sacrificing move.");
            case ColEvent:  return tr("Event / tournament name from "
                                      "the PGN tag.");
            default:        return {};
        }
    }
    return {};
}

} // namespace chesserazade::analyzer

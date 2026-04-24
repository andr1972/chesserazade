// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Table model over the output of `chesserazade::index_games`.
///
/// Five columns — Date / White / Black / Result / Event — map
/// directly onto `PgnGameHeader` fields. The model does not
/// own the underlying `std::vector<PgnGameHeader>`: the owner
/// (the enclosing view) loads the PGN once, indexes it once,
/// and hands the model a pointer that outlives it.
#pragma once

#include <chesserazade/game_index.hpp>

#include <QAbstractTableModel>

#include <vector>

namespace chesserazade::analyzer {

class GameListModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column : int {
        ColDate = 0,
        ColWhite,
        ColBlack,
        ColResult,
        ColEco,    ///< Optional ECO code; blank when absent.
        ColPlies,  ///< Half-move count of the main line.
        ColEnd,    ///< "#" for mate, "=" for stalemate, blank otherwise.
        ColUP,     ///< Under-promotion letters (e.g. "N", "NR") or blank.
        ColKF,     ///< Knight-fork count or blank.
        ColSac,    ///< Biggest piece drop (raw) + recovery %, or blank.
        ColEvent,
        ColumnCount,
    };

    explicit GameListModel(QObject* parent = nullptr);

    /// Point the model at a new index. The vector must outlive
    /// the model; pass `nullptr` to clear.
    void set_games(const std::vector<GameRecord>* games);

    /// Direct access to the underlying record. Used by filter
    /// logic that needs fields not exposed as columns (e.g.
    /// the "winning sacrifice" filter needs to cross-reference
    /// sacrifice plies against the game result). Returns
    /// nullptr for out-of-range rows.
    [[nodiscard]] const GameRecord* record_at(int row) const noexcept;

    [[nodiscard]] int rowCount(const QModelIndex& parent
                               = QModelIndex{}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent
                                  = QModelIndex{}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& idx,
                                int role) const override;
    [[nodiscard]] QVariant headerData(int section,
                                      Qt::Orientation orientation,
                                      int role) const override;

private:
    const std::vector<GameRecord>* games_ = nullptr;
};

} // namespace chesserazade::analyzer

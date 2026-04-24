// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `GameListView` — a widget that loads a multi-game PGN,
/// indexes it via `chesserazade::index_games`, and exposes the
/// games as a sortable QTableView. Double-click (or Enter) on
/// a row emits `game_chosen` with the game's raw PGN text
/// sliced out of the underlying file bytes.
///
/// 1.3.5 wires this view into MainWindow as the central widget
/// after a successful fetch; 1.3.6 consumes `game_chosen` to
/// populate the board + move-list pane.
#pragma once

#include <chesserazade/game_index.hpp>

#include <QString>
#include <QWidget>

#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QModelIndex;
class QTableView;

namespace chesserazade::analyzer {

class GameListModel;
class GameFilterProxy;

class GameListView final : public QWidget {
    Q_OBJECT
public:
    explicit GameListView(QWidget* parent = nullptr);

    /// Read `pgn_path`, index it, and populate the table.
    /// Returns false on read / index failure; the status line
    /// carries the reason.
    bool load(const QString& pgn_path);

signals:
    /// A game was activated (double-click / Enter). `pgn_text`
    /// is a copy of that one game's bytes; `header_label` is
    /// a short "White — Black, Date" string for the tab title.
    void game_chosen(const QString& pgn_text,
                     const QString& header_label);

protected:
    /// Swallow Escape on either filter field so it clears that
    /// field instead of propagating to the main window.
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void on_activated(const QModelIndex& idx);
    void on_name_filter_changed();
    void on_year_filter_changed();
    /// Rebuild the year combo to show only years that still
    /// have at least one game under the current name filter.
    /// The currently-typed year text is preserved; if it no
    /// longer matches anything, the combo goes blank.
    void refresh_year_combo();
    [[nodiscard]] int source_row(const QModelIndex& idx) const;

    std::string pgn_bytes_;
    std::vector<GameRecord> games_;

    QTableView*    table_   = nullptr;
    GameListModel* model_   = nullptr;
    GameFilterProxy* proxy_ = nullptr;
    QLineEdit*     name_filter_ = nullptr;
    QComboBox*     year_filter_ = nullptr;
    QCheckBox*     mate_filter_ = nullptr;
    QCheckBox*     up_filter_   = nullptr;
    QLabel*        status_  = nullptr;
    QString        source_path_;
};

} // namespace chesserazade::analyzer

// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Top-level window for `chesserazade-analyzer`.
///
/// The 1.3.3 skeleton only hosts an empty central widget, a File
/// and a Help menu. Subsequent sub-etaps populate it: 1.3.4 adds
/// a Fetch dialog, 1.3.5 a game-list view, 1.3.6 the board +
/// move-list pane, 1.3.7 the Solve panel, 1.3.8 the live search
/// tree.
#pragma once

#include <QMainWindow>

class QAction;
class QStackedWidget;

namespace chesserazade::analyzer {

class GameListView;
class GameView;
class SolvePanel;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void build_menus();
    void show_about();
    void open_fetch_dialog();
    void on_game_chosen(const QString& pgn_text,
                        const QString& header_label,
                        int target_ply);
    void on_back_to_list();
    void on_solve_requested();
    void on_back_to_game_view();
    /// "Bookmarks → Add…" — capture the currently-displayed
    /// position (GameView's current ply, or the snapshot taken
    /// at solve-entry time when the Solve panel is on top),
    /// show the AddBookmarkDialog, and persist.
    void on_add_bookmark();
    /// "Bookmarks → Browse…" — pop the list dialog, on accept
    /// locate the zip in the cache (downloading if needed),
    /// resolve the saved game via `resolve_game`, switch the
    /// view to GameView seeked to the saved ply.
    void on_browse_bookmarks();
    /// "Game → Return to list" — drop back to the GameListView
    /// without touching its model, so the user's filters and
    /// sort column survive the round-trip. Disabled on the
    /// game list itself; enabled in the board view and the
    /// Solve panel.
    void on_return_to_list();
    void update_bookmark_action_enabled();

    QStackedWidget* stack_    = nullptr;
    GameListView* game_list_  = nullptr;
    GameView*     game_view_  = nullptr;
    SolvePanel*   solve_panel_ = nullptr;
    QString loaded_pgn_path_;
    QAction* add_bookmark_action_ = nullptr;
    QAction* return_to_list_action_ = nullptr;
    /// Ply the user was at in GameView when they clicked
    /// "Solve from here". SolvePanel itself has no concept of
    /// game-plies — it browses a search tree — so we freeze
    /// this number at the moment of transition and reuse it for
    /// bookmarks created while the Solve panel is on top.
    int solve_source_ply_ = 0;
};

} // namespace chesserazade::analyzer

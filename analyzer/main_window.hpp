/// Top-level window for `chesserazade-analyzer`.
///
/// The 1.3.3 skeleton only hosts an empty central widget, a File
/// and a Help menu. Subsequent sub-etaps populate it: 1.3.4 adds
/// a Fetch dialog, 1.3.5 a game-list view, 1.3.6 the board +
/// move-list pane, 1.3.7 the Solve panel, 1.3.8 the live search
/// tree.
#pragma once

#include <QMainWindow>

class QStackedWidget;

namespace chesserazade::analyzer {

class GameListView;
class GameView;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void build_menus();
    void show_about();
    void open_fetch_dialog();
    void on_game_chosen(const QString& pgn_text,
                        const QString& header_label);
    void on_back_to_list();

    QStackedWidget* stack_   = nullptr;
    GameListView* game_list_ = nullptr;
    GameView*     game_view_ = nullptr;
    QString loaded_pgn_path_;
};

} // namespace chesserazade::analyzer

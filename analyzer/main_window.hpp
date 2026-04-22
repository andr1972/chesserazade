/// Top-level window for `chesserazade-analyzer`.
///
/// The 1.3.3 skeleton only hosts an empty central widget, a File
/// and a Help menu. Subsequent sub-etaps populate it: 1.3.4 adds
/// a Fetch dialog, 1.3.5 a game-list view, 1.3.6 the board +
/// move-list pane, 1.3.7 the Solve panel, 1.3.8 the live search
/// tree.
#pragma once

#include <QMainWindow>

namespace chesserazade::analyzer {

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void build_menus();
    void show_about();
};

} // namespace chesserazade::analyzer

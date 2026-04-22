#include "main_window.hpp"

#include "fetch_dialog.hpp"
#include "game_list_view.hpp"

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

namespace chesserazade::analyzer {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("chesserazade analyzer"));
    resize(1024, 720);

    game_list_ = new GameListView(this);
    connect(game_list_, &GameListView::game_chosen,
            this, &MainWindow::on_game_chosen);
    setCentralWidget(game_list_);

    build_menus();
}

void MainWindow::build_menus() {
    auto* file_menu = menuBar()->addMenu(tr("&File"));

    auto* fetch_action = file_menu->addAction(tr("&Fetch..."));
    fetch_action->setShortcut(QKeySequence::Open);
    connect(fetch_action, &QAction::triggered,
            this, &MainWindow::open_fetch_dialog);

    file_menu->addSeparator();

    auto* quit_action = file_menu->addAction(tr("&Quit"));
    quit_action->setShortcut(QKeySequence::Quit);
    connect(quit_action, &QAction::triggered,
            qApp, &QApplication::quit);

    auto* help_menu = menuBar()->addMenu(tr("&Help"));
    auto* about_action = help_menu->addAction(tr("&About"));
    connect(about_action, &QAction::triggered,
            this, &MainWindow::show_about);
}

void MainWindow::open_fetch_dialog() {
    FetchDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString pgn = dlg.selected_pgn_path();
    if (pgn.isEmpty()) return;
    loaded_pgn_path_ = pgn;
    if (game_list_ != nullptr && game_list_->load(pgn)) {
        statusBar()->showMessage(
            tr("Loaded %1").arg(QFileInfo(pgn).fileName()));
    } else {
        statusBar()->showMessage(
            tr("Failed to index %1").arg(pgn));
    }
}

void MainWindow::on_game_chosen(const QString& /*pgn_text*/,
                                const QString& header_label) {
    // 1.3.6 will push the game's move list + starting position
    // into a board view; for 1.3.5 we acknowledge the selection
    // via the status bar so the indexing + selection pipeline is
    // end-to-end testable.
    statusBar()->showMessage(tr("Selected: %1").arg(header_label));
}

void MainWindow::show_about() {
    QMessageBox::about(
        this,
        tr("About chesserazade analyzer"),
        tr("<h3>chesserazade analyzer</h3>"
           "<p>Graphical PGN browser and search analyzer,"
           " part of the chesserazade project.</p>"
           "<p>Version 1.3.0-dev (1.3.5 — fetch + game list)</p>"
           "<p>Author: Andrzej Borucki</p>"));
}

} // namespace chesserazade::analyzer

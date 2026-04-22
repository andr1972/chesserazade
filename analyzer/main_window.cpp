#include "main_window.hpp"

#include "fetch_dialog.hpp"
#include "game_list_view.hpp"
#include "game_view.hpp"
#include "solve_panel.hpp"

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStatusBar>

namespace chesserazade::analyzer {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("chesserazade analyzer"));
    resize(1024, 720);

    stack_ = new QStackedWidget(this);
    game_list_   = new GameListView(stack_);
    game_view_   = new GameView(stack_);
    solve_panel_ = new SolvePanel(stack_);
    connect(game_list_, &GameListView::game_chosen,
            this, &MainWindow::on_game_chosen);
    connect(game_view_, &GameView::back_requested,
            this, &MainWindow::on_back_to_list);
    connect(game_view_, &GameView::solve_requested,
            this, &MainWindow::on_solve_requested);
    connect(solve_panel_, &SolvePanel::back_requested,
            this, &MainWindow::on_back_to_game_view);
    stack_->addWidget(game_list_);
    stack_->addWidget(game_view_);
    stack_->addWidget(solve_panel_);
    stack_->setCurrentWidget(game_list_);
    setCentralWidget(stack_);

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

void MainWindow::on_game_chosen(const QString& pgn_text,
                                const QString& header_label) {
    if (game_view_->load_pgn(pgn_text, header_label)) {
        stack_->setCurrentWidget(game_view_);
        statusBar()->showMessage(tr("Viewing: %1").arg(header_label));
    } else {
        statusBar()->showMessage(tr("Failed to parse selected game"));
    }
}

void MainWindow::on_back_to_list() {
    stack_->setCurrentWidget(game_list_);
    statusBar()->clearMessage();
}

void MainWindow::on_solve_requested() {
    solve_panel_->set_position(
        game_view_->current_board(),
        game_view_->header_label());
    stack_->setCurrentWidget(solve_panel_);
}

void MainWindow::on_back_to_game_view() {
    stack_->setCurrentWidget(game_view_);
}

void MainWindow::show_about() {
    QMessageBox::about(
        this,
        tr("About chesserazade analyzer"),
        tr("<h3>chesserazade analyzer</h3>"
           "<p>Graphical PGN browser and search analyzer,"
           " part of the chesserazade project.</p>"
           "<p>Version 1.3.0-dev (1.3.7 — solve panel)</p>"
           "<p>Author: Andrzej Borucki</p>"));
}

} // namespace chesserazade::analyzer

#include "main_window.hpp"

#include "add_bookmark_dialog.hpp"
#include "bookmarks.hpp"
#include "fetch_dialog.hpp"
#include "game_list_view.hpp"
#include "game_view.hpp"
#include "solve_panel.hpp"

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QKeySequence>
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

    connect(stack_, &QStackedWidget::currentChanged,
            this, [this](int) { update_bookmark_action_enabled(); });
    update_bookmark_action_enabled();
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

    auto* bookmarks_menu = menuBar()->addMenu(tr("&Bookmarks"));
    add_bookmark_action_ = bookmarks_menu->addAction(tr("&Add…"));
    add_bookmark_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    connect(add_bookmark_action_, &QAction::triggered,
            this, &MainWindow::on_add_bookmark);

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
        // Whatever page the user was on (game view, solve
        // panel), a fresh fetch should surface the newly-
        // indexed list so they can pick a game. Otherwise they
        // get stuck on the previously-open board with no
        // obvious way back to the list.
        stack_->setCurrentWidget(game_list_);
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
    // Freeze the source ply now — SolvePanel itself doesn't
    // know about game-history plies, so a bookmark created
    // while in the Solve panel pins the position the user
    // opened it from, not wherever their tree-exploration
    // selection happens to be.
    solve_source_ply_ = game_view_->current_ply();
    solve_panel_->set_position(
        game_view_->current_board(),
        game_view_->header_label());
    stack_->setCurrentWidget(solve_panel_);
}

void MainWindow::update_bookmark_action_enabled() {
    if (add_bookmark_action_ == nullptr) return;
    QWidget* cur = stack_->currentWidget();
    add_bookmark_action_->setEnabled(
        cur == game_view_ || cur == solve_panel_);
}

void MainWindow::on_add_bookmark() {
    QWidget* cur = stack_->currentWidget();
    if (cur != game_view_ && cur != solve_panel_) return;

    Bookmark bm;
    bm.zip = loaded_pgn_path_.isEmpty()
        ? QString{}
        : QFileInfo(loaded_pgn_path_).completeBaseName()
              + QStringLiteral(".zip");
    bm.white  = game_view_->tag_white();
    bm.black  = game_view_->tag_black();
    bm.date   = game_view_->tag_date();
    bm.event  = game_view_->tag_event();
    bm.round  = game_view_->tag_round();
    bm.ply = (cur == game_view_)
        ? game_view_->current_ply()
        : solve_source_ply_;

    auto existing = load_bookmarks().value_or(std::vector<Bookmark>{});

    AddBookmarkDialog dlg(bm, folders_in_use(existing), this);
    if (dlg.exec() != QDialog::Accepted) return;

    existing.push_back(dlg.result());
    if (!save_bookmarks(existing)) {
        QMessageBox::warning(this, tr("Bookmarks"),
            tr("Could not write %1.").arg(bookmarks_file_path()));
        return;
    }
    statusBar()->showMessage(tr("Bookmark saved."), 3000);
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
           "<p>Version 1.3.0</p>"
           "<p>Author: Andrzej Borucki</p>"));
}

} // namespace chesserazade::analyzer

#include "main_window.hpp"

#include "add_bookmark_dialog.hpp"
#include "bookmarks.hpp"
#include "bookmarks_dialog.hpp"
#include "fetch_dialog.hpp"
#include "game_list_view.hpp"
#include "game_view.hpp"
#include "pgn_cache.hpp"
#include "solve_panel.hpp"

#include <chesserazade/pgn_index.hpp>

#include <QFile>

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

    auto* game_menu = menuBar()->addMenu(tr("&Game"));
    return_to_list_action_ =
        game_menu->addAction(tr("&Return to list"));
    return_to_list_action_->setShortcut(
        QKeySequence(QStringLiteral("Ctrl+L")));
    connect(return_to_list_action_, &QAction::triggered,
            this, &MainWindow::on_return_to_list);

    auto* bookmarks_menu = menuBar()->addMenu(tr("&Bookmarks"));
    add_bookmark_action_ = bookmarks_menu->addAction(tr("&Add…"));
    add_bookmark_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    connect(add_bookmark_action_, &QAction::triggered,
            this, &MainWindow::on_add_bookmark);

    auto* browse_bookmarks_action =
        bookmarks_menu->addAction(tr("&Browse…"));
    browse_bookmarks_action->setShortcut(
        QKeySequence(QStringLiteral("Ctrl+B")));
    connect(browse_bookmarks_action, &QAction::triggered,
            this, &MainWindow::on_browse_bookmarks);

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
    QWidget* cur = stack_->currentWidget();
    if (add_bookmark_action_ != nullptr) {
        add_bookmark_action_->setEnabled(
            cur == game_view_ || cur == solve_panel_);
    }
    if (return_to_list_action_ != nullptr) {
        return_to_list_action_->setEnabled(cur != game_list_);
    }
}

void MainWindow::on_return_to_list() {
    if (stack_->currentWidget() == game_list_) return;
    // The list's model, filter text, and sort column are all
    // held on the view and untouched by this switch — we just
    // pop back.
    stack_->setCurrentWidget(game_list_);
    statusBar()->clearMessage();
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

void MainWindow::on_browse_bookmarks() {
    BookmarksDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    const auto picked = dlg.chosen();
    if (!picked) return;
    const Bookmark& bm = *picked;

    if (bm.zip.isEmpty()) {
        QMessageBox::warning(this, tr("Bookmark"),
            tr("Bookmark has no source archive filename."));
        return;
    }

    const auto pgn_path = ensure_pgn(bm.zip, this);
    if (!pgn_path) return; // user cancelled or error already shown

    QFile f(*pgn_path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Bookmark"),
            tr("Could not read %1.").arg(*pgn_path));
        return;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    const std::string_view all(bytes.constData(),
                               static_cast<std::size_t>(bytes.size()));
    const auto headers = index_games(all);

    const auto match = resolve_game(bm, headers);
    if (!match) {
        QMessageBox::warning(this, tr("Bookmark"),
            tr("Could not locate the saved game in %1.\n"
               "The archive may have changed since the bookmark "
               "was made, or the bookmark's header fields may be "
               "ambiguous.").arg(bm.zip));
        return;
    }

    const PgnGameHeader& h = headers[*match];
    if (h.offset + h.length > static_cast<std::size_t>(bytes.size())) return;
    const QString pgn_text = QString::fromUtf8(
        bytes.constData() + h.offset,
        static_cast<qsizetype>(h.length));
    const QString label = bm.label.isEmpty()
        ? QStringLiteral("%1 — %2, %3")
            .arg(QString::fromStdString(h.white),
                 QString::fromStdString(h.black),
                 QString::fromStdString(h.date))
        : bm.label;

    // Also refresh the game-list so "back to list" from the
    // game view lands on the archive the bookmark came from,
    // not whatever was previously loaded.
    loaded_pgn_path_ = *pgn_path;
    if (game_list_ != nullptr) game_list_->load(*pgn_path);

    if (!game_view_->load_pgn(pgn_text, label)) {
        QMessageBox::warning(this, tr("Bookmark"),
            tr("Found the game but could not parse it."));
        return;
    }
    game_view_->go_to_ply(bm.ply);

    // If the user opened the browser from the Solve panel,
    // they probably still want to be analyzing — just the new
    // bookmark's position. Re-seed the solve panel with the
    // new board and keep the stack where it was. From anywhere
    // else (game list, game view), land on the game view so
    // the position is immediately readable.
    if (stack_->currentWidget() == solve_panel_) {
        solve_source_ply_ = bm.ply;
        solve_panel_->set_position(game_view_->current_board(), label);
    } else {
        stack_->setCurrentWidget(game_view_);
    }
    statusBar()->showMessage(
        tr("Opened bookmark: %1").arg(bm.label), 3000);
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

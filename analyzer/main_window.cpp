#include "main_window.hpp"

#include "fetch_dialog.hpp"

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

    auto* placeholder = new QLabel(
        tr("chesserazade analyzer\n\n"
           "1.3.3 skeleton — File → Fetch and Help → About are the\n"
           "only live menu items yet. Game list, board view, and\n"
           "the search tree arrive in the following sub-etaps."),
        this);
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);

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
    statusBar()->showMessage(
        tr("Loaded: %1  (%2 bytes)")
            .arg(pgn)
            .arg(QFileInfo(pgn).size()));
    // 1.3.5 will open the game-list view here; for 1.3.4 we
    // just record the path and report it in the status bar so
    // the fetch flow is end-to-end testable.
}

void MainWindow::show_about() {
    QMessageBox::about(
        this,
        tr("About chesserazade analyzer"),
        tr("<h3>chesserazade analyzer</h3>"
           "<p>Graphical PGN browser and search analyzer,"
           " part of the chesserazade project.</p>"
           "<p>Version 1.3.0-dev (1.3.4 — fetch dialog wired)</p>"
           "<p>Author: Andrzej Borucki</p>"));
}

} // namespace chesserazade::analyzer

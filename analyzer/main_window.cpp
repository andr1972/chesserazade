#include "main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>

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
    fetch_action->setEnabled(false); // populated in 1.3.4

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

void MainWindow::show_about() {
    QMessageBox::about(
        this,
        tr("About chesserazade analyzer"),
        tr("<h3>chesserazade analyzer</h3>"
           "<p>Graphical PGN browser and search analyzer,"
           " part of the chesserazade project.</p>"
           "<p>Version 1.3.0-dev (skeleton, 1.3.3)</p>"
           "<p>Author: Andrzej Borucki</p>"));
}

} // namespace chesserazade::analyzer

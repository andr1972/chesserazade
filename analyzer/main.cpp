// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `chesserazade-analyzer` — Qt6 GUI entry point.
///
/// Thin shell: construct a `QApplication`, show the main
/// window, run the event loop. All logic lives in the
/// `analyzer::` namespace. The core engine is linked in as
/// `chesserazade_core` — UCI is for *other* GUIs; this one
/// talks to the engine directly so it can observe the search
/// tree.
#include "main_window.hpp"

#include "board/magic.hpp"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("chesserazade-analyzer");
    QApplication::setOrganizationName("chesserazade");

    // Same initialization chain as the CLI: pick the fastest
    // slider-attack implementation for this build + CPU.
    (void)chesserazade::init_slider_attacks();

    chesserazade::analyzer::MainWindow window;
    window.show();

    return app.exec();
}

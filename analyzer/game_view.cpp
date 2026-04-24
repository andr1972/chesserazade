// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include "game_view.hpp"

#include "board_widget.hpp"

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/pgn.hpp>
#include <chesserazade/san.hpp>

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QApplication>
#include <QClipboard>
#include <QListWidget>
#include <QMenu>
#include <QPoint>
#include <QPushButton>
#include <QSplitter>
#include <QString>
#include <QVBoxLayout>

#include <string>

namespace chesserazade::analyzer {

GameView::GameView(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(this);

    header_ = new QLabel(this);
    header_->setWordWrap(true);
    QFont hfont = header_->font();
    hfont.setBold(true);
    header_->setFont(hfont);
    layout->addWidget(header_);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    board_ = new BoardWidget(splitter);
    moves_ = new QListWidget(splitter);
    moves_->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(moves_, &QListWidget::currentRowChanged,
            this, &GameView::on_move_clicked);
    // Double-click on a ply is a shortcut for "click the move +
    // press Solve from here" — the common flow when the user
    // already knows the position they want to analyze.
    connect(moves_, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem* item) {
                if (item == nullptr) return;
                seek_to_ply(moves_->row(item));
                emit solve_requested();
            });

    // Right-click context menu: copy the clicked move in SAN
    // or the whole game as UCI source-destination pairs.
    moves_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(moves_, &QListWidget::customContextMenuRequested,
            this, &GameView::on_moves_context_menu);
    splitter->addWidget(board_);
    splitter->addWidget(moves_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, /*stretch=*/1);

    auto* action_row = new QHBoxLayout;
    auto* solve_btn = new QPushButton(tr("&Solve from here"), this);
    connect(solve_btn, &QPushButton::clicked,
            this, &GameView::solve_requested);
    action_row->addWidget(solve_btn);
    action_row->addStretch(1);
    layout->addLayout(action_row);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(status_);

    status_->setText(tr(
        "←/→ step through moves · Home/End jump to start/end · "
        "Esc returns to the game list"));
}

Board8x8Mailbox GameView::current_board() const {
    return display_board_;
}

void GameView::go_to_ply(int ply) {
    seek_to_ply(ply);
}

QString GameView::header_label() const {
    return header_label_;
}

bool GameView::load_pgn(const QString& pgn_text,
                        const QString& header_label) {
    header_->setText(header_label);
    header_label_ = header_label;
    moves_->clear();

    const std::string src = pgn_text.toStdString();
    auto parsed = parse_pgn(src);
    if (!parsed) {
        status_->setText(tr("PGN parse error: %1")
                             .arg(QString::fromStdString(parsed.error().message)));
        game_ = Game{};
        current_ply_ = 0;
        board_->set_position(
            *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN));
        return false;
    }

    Board8x8Mailbox start =
        parsed->starting_fen.has_value()
            ? *Board8x8Mailbox::from_fen(*parsed->starting_fen)
            : *Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    game_ = Game{std::move(start)};
    for (const Move& m : parsed->moves) {
        game_.play_move(m);
    }

    // STR tags — kept so MainWindow can mint a bookmark that
    // resolves back to this game across cache rebuilds. Missing
    // tags fall back to empty; the resolver treats empty as
    // "any" on both sides of the match.
    auto tag = [&](std::string_view key) -> QString {
        auto v = parsed->tag(key);
        return v ? QString::fromStdString(*v) : QString{};
    };
    tag_white_ = tag("White");
    tag_black_ = tag("Black");
    tag_date_  = tag("Date");
    tag_event_ = tag("Event");
    tag_round_ = tag("Round");

    rebuild_move_list();
    seek_to_ply(static_cast<int>(game_.ply_count()));
    status_->setText(
        tr("%1 plies loaded. ←/→ step, Home/End jump, Esc returns.")
            .arg(game_.ply_count()));
    setFocus();
    return true;
}

void GameView::rebuild_move_list() {
    moves_->blockSignals(true);
    moves_->clear();

    // Add a synthetic "start" item so the user can also pick the
    // initial position from the list.
    moves_->addItem(tr("(start)"));

    // Replay each move from the starting position to generate
    // SAN — the serialiser needs the pre-move board.
    Board8x8Mailbox b{static_cast<const Board8x8Mailbox&>(
        game_.starting_position())};

    const auto& mvs = game_.moves();
    for (std::size_t i = 0; i < mvs.size(); ++i) {
        const Move& m = mvs[i];
        const std::string san = to_san(b, m);

        QString label;
        const std::size_t fullmove = i / 2 + 1;
        if (i % 2 == 0) {
            label = QStringLiteral("%1. %2")
                        .arg(fullmove)
                        .arg(QString::fromStdString(san));
        } else {
            label = QStringLiteral("%1… %2")
                        .arg(fullmove)
                        .arg(QString::fromStdString(san));
        }
        moves_->addItem(label);
        b.make_move(m);
    }
    moves_->blockSignals(false);
}

void GameView::seek_to_ply(int ply) {
    const int max_ply = static_cast<int>(game_.ply_count());
    if (ply < 0) ply = 0;
    if (ply > max_ply) ply = max_ply;
    current_ply_ = ply;

    Board8x8Mailbox b{
        static_cast<const Board8x8Mailbox&>(game_.starting_position())};
    const auto& mvs = game_.moves();
    for (int i = 0; i < ply; ++i) {
        b.make_move(mvs[static_cast<std::size_t>(i)]);
    }
    display_board_ = b;
    board_->set_position(b);

    moves_->blockSignals(true);
    moves_->setCurrentRow(ply);  // row 0 == (start), row i == after ply i
    moves_->blockSignals(false);
}

void GameView::on_move_clicked(int row) {
    if (row < 0) return;
    seek_to_ply(row);
}

void GameView::on_moves_context_menu(const QPoint& pos) {
    QListWidgetItem* item = moves_->itemAt(pos);
    // Row 0 is the synthetic "(start)" entry — there's no
    // single move to copy, but the whole-game actions are
    // still valid; the single-move action greys out at row 0.
    const int row = (item != nullptr) ? moves_->row(item) : -1;

    QMenu menu(this);
    QAction* copy_one = menu.addAction(tr("Copy move"));
    QAction* copy_pgn = menu.addAction(tr("Copy all moves (PGN)"));
    QAction* copy_uci = menu.addAction(tr("Copy all moves (UCI)"));

    if (row <= 0) copy_one->setEnabled(false);

    QAction* picked = menu.exec(moves_->viewport()->mapToGlobal(pos));
    if (picked == nullptr) return;

    QClipboard* cb = QApplication::clipboard();

    if (picked == copy_one) {
        // Single-move format with move number:
        //   white moves get "N.SAN"  (e.g. "4.Kf1")
        //   black moves get "N...SAN" (e.g. "4...Bc5")
        // Row index = ply (row 1 is ply 1 = white's first move).
        const int ply = row;
        const auto& mvs = game_.moves();
        if (ply <= 0 || ply > static_cast<int>(mvs.size())) return;
        Board8x8Mailbox b{static_cast<const Board8x8Mailbox&>(
            game_.starting_position())};
        for (int j = 0; j + 1 < ply; ++j) {
            b.make_move(mvs[static_cast<std::size_t>(j)]);
        }
        const Move& m = mvs[static_cast<std::size_t>(ply - 1)];
        const QString san = QString::fromStdString(to_san(b, m));
        const int fullmove = (ply + 1) / 2;
        const bool white = (ply % 2 == 1);
        const QString out = white
            ? QStringLiteral("%1.%2").arg(fullmove).arg(san)
            : QStringLiteral("%1...%2").arg(fullmove).arg(san);
        cb->setText(out);
        return;
    }

    if (picked == copy_pgn) {
        // Standard PGN move text: "1.e4 e5 2.Nf3 Nc6 …".
        // Move numbers appear before each white half-move; black
        // half-moves are bare SAN. No tags, no result, no
        // wrapping — just the move list, ready to paste into a
        // PGN body or a chat message.
        QString out;
        const auto& mvs = game_.moves();
        Board8x8Mailbox b{static_cast<const Board8x8Mailbox&>(
            game_.starting_position())};
        for (std::size_t i = 0; i < mvs.size(); ++i) {
            if (i % 2 == 0) {
                if (!out.isEmpty()) out += QLatin1Char(' ');
                out += QStringLiteral("%1.").arg(i / 2 + 1);
            } else {
                out += QLatin1Char(' ');
            }
            out += QString::fromStdString(to_san(b, mvs[i]));
            b.make_move(mvs[i]);
        }
        cb->setText(out);
        return;
    }

    if (picked == copy_uci) {
        QString out;
        const auto& mvs = game_.moves();
        for (std::size_t i = 0; i < mvs.size(); ++i) {
            if (i > 0) out += QLatin1Char(' ');
            out += QString::fromStdString(to_uci(mvs[i]));
        }
        cb->setText(out);
    }
}

void GameView::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_Left:
            seek_to_ply(current_ply_ - 1);
            e->accept();
            return;
        case Qt::Key_Right:
            seek_to_ply(current_ply_ + 1);
            e->accept();
            return;
        case Qt::Key_Home:
            seek_to_ply(0);
            e->accept();
            return;
        case Qt::Key_End:
            seek_to_ply(static_cast<int>(game_.ply_count()));
            e->accept();
            return;
        case Qt::Key_Escape:
            emit back_requested();
            e->accept();
            return;
        default:
            break;
    }
    QWidget::keyPressEvent(e);
}

} // namespace chesserazade::analyzer

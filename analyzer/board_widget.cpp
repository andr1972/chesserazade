// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include "board_widget.hpp"

#include <chesserazade/fen.hpp>

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QRectF>
#include <QString>

namespace chesserazade::analyzer {

namespace {

/// Unicode code points for the 12 piece glyphs, indexed
/// `[Color][PieceType - 1]`. White pieces are U+2654..U+2659,
/// black pieces U+265A..U+265F.
[[nodiscard]] QString glyph_for(const Piece& p) {
    if (p.type == PieceType::None) return {};
    // PieceType enum is ordered Pawn=1, Knight=2, Bishop=3,
    // Rook=4, Queen=5, King=6 — NOT the Unicode code-point
    // order (♔=King first). Arrays here follow the enum so
    // `WHITE[PieceType - 1]` lines up correctly.
    static constexpr char32_t WHITE[] =
        {U'♙', U'♘', U'♗', U'♖', U'♕', U'♔'};
    static constexpr char32_t BLACK[] =
        {U'♟', U'♞', U'♝', U'♜', U'♛', U'♚'};
    const std::size_t idx =
        static_cast<std::size_t>(p.type) - 1;  // skip PieceType::None
    if (idx >= 6) return {};
    const char32_t cp = (p.color == Color::White)
                           ? WHITE[idx]
                           : BLACK[idx];
    return QString::fromUcs4(reinterpret_cast<const char32_t*>(&cp), 1);
}

constexpr QColor LIGHT(0xf0, 0xd9, 0xb5);
constexpr QColor DARK (0xb5, 0x88, 0x63);
constexpr QColor COORD(0x5a, 0x4a, 0x3a);

} // namespace

BoardWidget::BoardWidget(QWidget* parent)
    : QWidget(parent),
      board_(*Board8x8Mailbox::from_fen(STARTING_POSITION_FEN)) {
    setMinimumSize(minimumSizeHint());
    setFocusPolicy(Qt::StrongFocus);
}

void BoardWidget::set_position(const Board8x8Mailbox& board) {
    board_ = board;
    update();
}

void BoardWidget::paintEvent(QPaintEvent* /*e*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    // Keep the board square and centred.
    const int side = std::min(width(), height());
    const qreal cell = side / 8.0;
    const qreal ox = (width()  - side) / 2.0;
    const qreal oy = (height() - side) / 2.0;

    // Piece glyphs look right at ~0.7× the cell height on most
    // systems; Qt pads font metrics so we nudge to 0.72.
    QFont font = p.font();
    font.setPointSizeF(cell * 0.72);
    p.setFont(font);

    for (int rank = 7; rank >= 0; --rank) {
        for (int file = 0; file < 8; ++file) {
            const bool light = ((file + rank) % 2) == 1;
            const QRectF sq(ox + file * cell,
                            oy + (7 - rank) * cell,
                            cell, cell);
            p.fillRect(sq, light ? LIGHT : DARK);

            const Square sq_idx = static_cast<Square>(rank * 8 + file);
            const Piece piece = board_.piece_at(sq_idx);
            if (piece.type == PieceType::None) continue;
            const QString text = glyph_for(piece);
            p.setPen(Qt::black);
            p.drawText(sq, Qt::AlignCenter, text);
        }
    }

    // Coordinate labels along the left and bottom edges.
    QFont small = font;
    small.setPointSizeF(cell * 0.2);
    p.setFont(small);
    p.setPen(COORD);
    for (int rank = 0; rank < 8; ++rank) {
        const QRectF lbl(ox, oy + (7 - rank) * cell,
                         cell * 0.25, cell * 0.25);
        p.drawText(lbl, Qt::AlignCenter,
                   QString::number(rank + 1));
    }
    for (int file = 0; file < 8; ++file) {
        const QRectF lbl(ox + file * cell + cell * 0.75,
                         oy + 7 * cell + cell * 0.75,
                         cell * 0.25, cell * 0.25);
        const char c = static_cast<char>('a' + file);
        p.drawText(lbl, Qt::AlignCenter, QString(QChar::fromLatin1(c)));
    }
}

} // namespace chesserazade::analyzer

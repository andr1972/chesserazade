/// `BoardWidget` — a read-only 8×8 chess board display.
///
/// Renders the current position with Unicode figurines
/// (♔♕♖♗♘♙ for white, ♚♛♜♝♞♟ for black). Font size scales with
/// the widget's shortest side so the piece glyphs fill the
/// square cleanly at any window size. No interaction in 1.3.6
/// — navigation is driven externally via `set_position`.
///
/// White is always drawn at the bottom; a 1.3.x polish etap may
/// later add board-flip, but for "browse a PGN" the fixed
/// orientation matches what every chess GUI does when following
/// a named game.
#pragma once

#include "board/board8x8_mailbox.hpp"

#include <QWidget>

namespace chesserazade::analyzer {

class BoardWidget final : public QWidget {
    Q_OBJECT
public:
    explicit BoardWidget(QWidget* parent = nullptr);

    /// Replace the displayed position and repaint.
    void set_position(const Board8x8Mailbox& board);

    [[nodiscard]] QSize sizeHint() const override { return {480, 480}; }
    [[nodiscard]] QSize minimumSizeHint() const override
        { return {240, 240}; }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    Board8x8Mailbox board_;
};

} // namespace chesserazade::analyzer

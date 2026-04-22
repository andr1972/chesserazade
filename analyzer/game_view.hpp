/// `GameView` — pane showing a single PGN game as board +
/// move list.
///
/// Input is the raw PGN text for one game (as sliced out of a
/// multi-game file by `GameListView`). `GameView` parses it,
/// builds an internal `Game`, and lets the user step through
/// the plies with ←/→/Home/End or by clicking a move.
///
/// The board is a `BoardWidget`; the move list is a
/// `QListWidget`, one row per ply, labelled in long SAN form
/// with move numbers. Navigation simply seeks to the chosen
/// ply — each seek replays the game from `starting_position()`
/// so that captures and state (castling rights, EP square,
/// zobrist) are always consistent with what `make_move` / the
/// engine see.
#pragma once

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/game.hpp>

#include <QString>
#include <QWidget>

class QLabel;
class QListWidget;

namespace chesserazade::analyzer {

class BoardWidget;

class GameView final : public QWidget {
    Q_OBJECT
public:
    explicit GameView(QWidget* parent = nullptr);

    /// Parse `pgn_text` and populate the view. `header_label`
    /// is the short "White — Black, Date" string shown above
    /// the board. Returns false if the PGN is malformed; in
    /// that case the view is cleared and the status line
    /// carries the error.
    bool load_pgn(const QString& pgn_text,
                  const QString& header_label);

    /// Board as rendered for the current ply — consumed by
    /// MainWindow when opening the solve panel so the engine
    /// starts from exactly what the user is looking at.
    [[nodiscard]] Board8x8Mailbox current_board() const;

    /// "White — Black, Date" label describing the loaded game.
    [[nodiscard]] QString header_label() const;

signals:
    /// User pressed Escape or clicked "Back to game list" —
    /// MainWindow switches the central widget accordingly.
    void back_requested();

    /// User clicked "Solve" — MainWindow opens the solve panel
    /// against `current_board()`.
    void solve_requested();

protected:
    void keyPressEvent(QKeyEvent* e) override;

private:
    void seek_to_ply(int ply);
    void on_move_clicked(int row);
    void rebuild_move_list();

    BoardWidget* board_    = nullptr;
    QListWidget* moves_    = nullptr;
    QLabel*      header_   = nullptr;
    QLabel*      status_   = nullptr;

    Game game_;        // parsed PGN
    int current_ply_ = 0;   // 0 = starting position; i = after game_.moves()[i-1]
    QString header_label_;
    Board8x8Mailbox display_board_ =
        *Board8x8Mailbox::from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
};

} // namespace chesserazade::analyzer

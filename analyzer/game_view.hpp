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

    /// Ply the user is currently looking at (0 = starting pos).
    /// Read by MainWindow when the user asks to bookmark.
    [[nodiscard]] int current_ply() const noexcept { return current_ply_; }

    /// Jump the view to `ply`, clamped to the legal range.
    /// Called by MainWindow when a bookmark is opened, so the
    /// GameView lands on the saved position right after
    /// `load_pgn` replays the moves.
    void go_to_ply(int ply);

    /// STR tags parsed out of the current PGN — surfaced for
    /// bookmark creation so the saved row carries enough header
    /// to re-resolve the game after cache rebuilds.
    [[nodiscard]] const QString& tag_white() const noexcept { return tag_white_; }
    [[nodiscard]] const QString& tag_black() const noexcept { return tag_black_; }
    [[nodiscard]] const QString& tag_date()  const noexcept { return tag_date_;  }
    [[nodiscard]] const QString& tag_event() const noexcept { return tag_event_; }
    [[nodiscard]] const QString& tag_round() const noexcept { return tag_round_; }

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
    QString tag_white_;
    QString tag_black_;
    QString tag_date_;
    QString tag_event_;
    QString tag_round_;
    Board8x8Mailbox display_board_ =
        *Board8x8Mailbox::from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
};

} // namespace chesserazade::analyzer

/// Game — a `Board` plus the sequence of moves that led to it.
///
/// `Board` is enough for the move generator and the search: they only
/// ever need the *current* position. But a user-facing session — a
/// PGN being read, a `repl` session, a game analyzer running over
/// an annotated file — cares about history: "what were the previous
/// moves?", "can I undo?", "save this as PGN". `Game` is the minimal
/// type that adds those capabilities without leaking them into the
/// engine core.
///
/// Design:
///   * Composition, not inheritance. `Game` owns a `Board8x8Mailbox`
///     (the concrete mailbox — bitboards arrive in 1.1) and a
///     `std::vector<Move>`.
///   * Every move goes through `play_move`, which delegates to
///     `Board::make_move` and appends to history. Every undo goes
///     through `undo_move`, which pops history and calls
///     `Board::unmake_move`.
///   * The *starting* position is remembered separately. PGN output
///     needs it (SetUp / FEN tags), and a user pressing "undo" all
///     the way home should land there, not on some default-constructed
///     empty board.
///
/// This class is deliberately thin — no iterators over "all positions
/// in this game", no SAN caching, no variations. Those are concerns
/// for callers (PGN writer, analyzer).
#pragma once

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/board.hpp>
#include <chesserazade/move.hpp>

#include <cstddef>
#include <vector>

namespace chesserazade {

class Game {
public:
    /// Start a game from the standard initial position.
    Game();

    /// Start a game from a given position. The board is taken by value
    /// and becomes both the *starting* and the *current* position.
    explicit Game(Board8x8Mailbox start);

    /// The position before any move was played. Useful for PGN output
    /// (if non-standard, it is emitted as a SetUp / FEN tag pair).
    [[nodiscard]] const Board& starting_position() const noexcept {
        return starting_;
    }

    /// The current (post-last-move) position.
    [[nodiscard]] const Board& current_position() const noexcept {
        return current_;
    }
    [[nodiscard]] Board& current_position() noexcept { return current_; }

    /// The sequence of moves played from the starting position to
    /// the current one, in order.
    [[nodiscard]] const std::vector<Move>& moves() const noexcept {
        return moves_;
    }

    /// Number of plies played since the starting position.
    [[nodiscard]] std::size_t ply_count() const noexcept {
        return moves_.size();
    }

    /// Play `m` on the current position and record it in history.
    /// The caller is responsible for legality; `Board::make_move`
    /// does not re-check the chess rules.
    void play_move(const Move& m);

    /// Undo the last move. Returns `false` if there is no history to
    /// undo (still at the starting position). Internally pops the
    /// move and calls `Board::unmake_move`.
    bool undo_move() noexcept;

    /// Reset to the starting position, discarding all history.
    void reset_to_start();

private:
    Board8x8Mailbox starting_;
    Board8x8Mailbox current_;
    std::vector<Move> moves_;
};

} // namespace chesserazade

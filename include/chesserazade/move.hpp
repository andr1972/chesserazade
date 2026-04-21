/// Move — the smallest thing the engine passes between layers.
///
/// For 0.1 a `Move` is just { from, to, promotion }. It is serialized to
/// UCI text (`e2e4`, `e7e8q`) for logging and CLI output; UCI parsing is
/// added alongside interactive play in 0.4.
///
/// In 0.2 this struct gains move-classification flags (double push,
/// en-passant, castling) and the moved/captured piece — fields that a
/// move generator needs to implement `make_move` / `unmake_move` without
/// consulting the board. The flag bits are intentionally left out of
/// 0.1 so that a reader of the scaffold sees only what is already used.
///
/// See https://www.chessprogramming.org/Moves for the classical
/// taxonomy of move kinds.
#pragma once

#include <chesserazade/types.hpp>

#include <string>

namespace chesserazade {

struct Move {
    Square from = Square::None;
    Square to = Square::None;
    /// Promotion target piece type. `PieceType::None` for a non-promotion
    /// move. Only Knight/Bishop/Rook/Queen are legal promotion targets.
    PieceType promotion = PieceType::None;

    friend constexpr bool operator==(const Move&, const Move&) = default;
};

/// Serialize a move as UCI long-algebraic notation.
///
/// Examples:
///   * `{e2, e4, None}`                 -> "e2e4"
///   * `{g1, f3, None}`                 -> "g1f3"
///   * `{e7, e8, Queen}`                -> "e7e8q"
///   * `{e1, g1, None}` (king-side O-O) -> "e1g1"
///
/// Castling uses the king's from/to squares (UCI "e1g1" / "e1c1"); this
/// is handled by the caller constructing the move correctly.
[[nodiscard]] std::string to_uci(const Move& m);

} // namespace chesserazade

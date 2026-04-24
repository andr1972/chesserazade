// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Move — the unit of play between layers.
///
/// A `Move` carries everything the engine needs to apply it to the
/// board (`make_move`) and then undo it (`unmake_move`), without the
/// move generator having to reach back into the board a second time.
///
/// Layout:
///   * `from`, `to` — origin and target squares.
///   * `promotion` — target piece type for promotion moves; `None`
///     otherwise.
///   * `kind` — the move classification (see `MoveKind`). Determines
///     the side-effects in `make_move` beyond moving the piece.
///   * `moved_piece` — the piece that was on `from`. Needed by
///     `unmake_move` to restore the from-square.
///   * `captured_piece` — the piece removed from the board. For a
///     normal capture it sits on `to`; for en-passant it sits on the
///     rank of the capturing pawn, not on `to`. `Piece::none()` for
///     all non-capturing moves. Needed by `unmake_move` and useful
///     for MVV-LVA move ordering (0.8+).
///
/// `to_uci` only inspects `from`, `to`, and `promotion`, so existing
/// tests that construct `Move{from, to, promotion}` work unchanged
/// with the new fields defaulted to `Quiet` / `Piece::none()`.
///
/// See https://www.chessprogramming.org/Encoding_Moves for the
/// classical encoding taxonomy.
#pragma once

#include <chesserazade/types.hpp>

#include <string>

namespace chesserazade {

/// Classification of a move's side-effects. Every `Move` carries one
/// of these so `make_move` knows what bookkeeping is required beyond
/// relocating the piece on `from`.
///
/// The eight kinds are mutually exclusive and exhaustive:
///   Quiet, DoublePush, KingsideCastle, QueensideCastle —
///     non-capturing moves.
///   Capture, EnPassant, Promotion, PromotionCapture —
///     moves that remove an enemy piece.
enum class MoveKind : std::uint8_t {
    Quiet,            ///< Normal non-capturing, non-special move.
    DoublePush,       ///< Pawn advances two squares; sets the EP target.
    KingsideCastle,   ///< O-O. King moves e→g; rook moves h→f.
    QueensideCastle,  ///< O-O-O. King moves e→c; rook moves a→d.
    Capture,          ///< Normal capture (not en-passant).
    EnPassant,        ///< En-passant capture; the captured pawn is NOT on `to`.
    Promotion,        ///< Pawn reaches the back rank without capture.
    PromotionCapture, ///< Pawn reaches the back rank while capturing.
};

struct Move {
    Square from = Square::None;
    Square to = Square::None;
    /// Promotion target. `PieceType::None` for non-promotion moves.
    /// Only Knight / Bishop / Rook / Queen are legal targets.
    PieceType promotion = PieceType::None;
    MoveKind kind = MoveKind::Quiet;
    Piece moved_piece = Piece::none();
    /// For en-passant the captured pawn is on the rank of `from`,
    /// at the file of `to`. Every other capture's piece sits on `to`.
    Piece captured_piece = Piece::none();

    friend constexpr bool operator==(const Move&, const Move&) = default;
};

/// Serialize a move as UCI long-algebraic notation.
///
/// Only `from`, `to`, and `promotion` are used; `kind` and the piece
/// fields are ignored. This means a default-constructed or zeroed
/// move with just the squares filled in produces valid UCI.
///
/// Examples:
///   * Quiet e2→e4        → "e2e4"
///   * Promotion e7→e8 Q  → "e7e8q"
///   * Kingside castle    → "e1g1"  (king's from/to squares)
[[nodiscard]] std::string to_uci(const Move& m);

} // namespace chesserazade

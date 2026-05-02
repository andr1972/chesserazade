// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Evaluator — static position assessment in centipawns.
///
/// This is the classical, educational evaluator described in the
/// Chess Programming Wiki's "Simplified Evaluation Function":
///
///   https://www.chessprogramming.org/Simplified_Evaluation_Function
///
/// Two ingredients:
///   1. **Material.** Each piece has a fixed centipawn value
///      (pawn=100, knight=320, bishop=330, rook=500, queen=900).
///      The king is not scored — it cannot be captured, and mate
///      is handled by the search layer, not the evaluator.
///   2. **Piece-square tables.** One 64-entry table per piece
///      type, indexed by the piece's square. The tables encode
///      classical positional wisdom — knights like the center,
///      rooks like the seventh rank, the king belongs in the
///      corner during the middlegame, pawns are worth more the
///      further they advance.
///
/// The evaluator is stateless: every call walks the 64 squares
/// and sums the two contributions. Return value is in centipawns
/// from the *side-to-move's* perspective (the convention expected
/// by a negamax search), so an `evaluate` of `+150` means the side
/// to move is up roughly a pawn and a half.
///
/// The tables are stored in white's orientation (a1 = index 0,
/// h8 = index 63). A black piece on square `s` reads
/// `table[s ^ 56]`, which mirrors the rank while keeping the file
/// — the cheapest possible PST flip.
#pragma once

#include <chesserazade/types.hpp>

#include <array>

namespace chesserazade {

class Board;

/// Centipawn piece values, classical. `KING` is a large sentinel,
/// never summed into a regular evaluation — it exists so the
/// search's mate scoring has a natural "how bad is losing the
/// king" reference if it ever needs one.
struct PieceValue {
    static constexpr int PAWN   = 100;
    static constexpr int KNIGHT = 320;
    static constexpr int BISHOP = 330;
    static constexpr int ROOK   = 500;
    static constexpr int QUEEN  = 900;
    static constexpr int KING   = 20000;
};

/// Centipawn value for the given piece type. `PieceType::None`
/// returns 0.
[[nodiscard]] constexpr int piece_value(PieceType pt) noexcept {
    switch (pt) {
        case PieceType::Pawn:   return PieceValue::PAWN;
        case PieceType::Knight: return PieceValue::KNIGHT;
        case PieceType::Bishop: return PieceValue::BISHOP;
        case PieceType::Rook:   return PieceValue::ROOK;
        case PieceType::Queen:  return PieceValue::QUEEN;
        case PieceType::King:   return PieceValue::KING;
        case PieceType::None:   return 0;
    }
    return 0;
}

/// Evaluate `board` in centipawns from the side-to-move's
/// perspective. Positive = good for side to move. The magnitude
/// is in *centipawns* so `+250` reads as "side to move is up
/// roughly 2.5 pawns".
[[nodiscard]] int evaluate(const Board& board) noexcept;

/// Single-piece contribution to the whole-board score, keyed
/// from white's perspective: material + PST for white pieces,
/// negated for black. Returns 0 for an empty square. Used by
/// board implementations that maintain an incremental running
/// sum on piece writes — add the new piece's contribution,
/// subtract the old one, and `evaluate(board)` stays in sync
/// at O(1).
[[nodiscard]] int piece_contribution(Piece p, Square sq) noexcept;

/// PSQT-only delta for a piece moving `from` → `to`. Returns the
/// purely-positional change (no material) from white's perspective,
/// i.e. negated for black. Cheaper than `piece_contribution(to) -
/// piece_contribution(from)` because it skips two material lookups
/// and one PST table dispatch — the same table is reused for
/// `from` and `to`. Used by move ordering.
[[nodiscard]] int psqt_delta(Piece p, Square from, Square to) noexcept;

} // namespace chesserazade

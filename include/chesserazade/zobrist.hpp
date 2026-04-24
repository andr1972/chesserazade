// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Zobrist hashing — 64-bit position keys.
///
/// A Zobrist key is the XOR of a fixed set of 64-bit random
/// constants, one per "position feature":
///
///   * for each (piece_type × color × square) there is one key;
///   * for each castling-rights combination there is one key
///     (we use 16 keys for the 4-bit KQkq mask);
///   * for each en-passant *file* there is one key (the rank is
///     determined by whose turn it is, so file alone suffices);
///   * one key XORed in when it is black to move.
///
/// The interesting property is that moving a piece from `A` to
/// `B` is `key ^= Z(piece, A) ^ Z(piece, B)`. Restoring the old
/// position means XORing the same constants back in — incremental
/// updates are trivially reversible.
///
/// Classical reference: Albert Zobrist, 1970. See
/// https://www.chessprogramming.org/Zobrist_Hashing for the full
/// treatment.
///
/// The random constants are seeded from a fixed seed at program
/// start so that two runs of the engine produce identical keys
/// — useful for reproducible tests and puzzle caches.
#pragma once

#include <chesserazade/board.hpp>
#include <chesserazade/types.hpp>

namespace chesserazade {

/// Zobrist random-constant table plus feature accessors. The
/// constants are initialized once at program start from a fixed
/// seed (`mt19937_64(0xCE55ErazadeULL)`) so keys are deterministic
/// across runs.
class Zobrist {
public:
    /// Key contribution for a piece on a square. Returns 0 for
    /// `PieceType::None`.
    [[nodiscard]] static ZobristKey piece(Piece p, Square sq) noexcept;

    /// Key contribution for the castling-rights combination.
    [[nodiscard]] static ZobristKey castling(CastlingRights cr) noexcept;

    /// Key contribution for the en-passant target. Only the file
    /// matters (the rank is derivable from the side to move); a
    /// `Square::None` EP target contributes 0.
    [[nodiscard]] static ZobristKey en_passant(Square ep) noexcept;

    /// Key contribution for the side to move. XORed in only when
    /// it is black's turn.
    [[nodiscard]] static ZobristKey black_to_move() noexcept;

    Zobrist() = delete;
};

/// Compute the full Zobrist key of `board` from scratch. Useful
/// for initializing the key in `from_fen`, for testing the
/// incremental-update path against a from-scratch recomputation,
/// and as a reference during debugging.
[[nodiscard]] ZobristKey compute_zobrist_key(const Board& board) noexcept;

} // namespace chesserazade

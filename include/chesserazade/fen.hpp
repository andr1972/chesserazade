// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// FEN — Forsyth-Edwards Notation I/O.
///
/// FEN is the ASCII serialization of a chess position:
///
///   "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
///    ^--- placement -----------------^ ^- ^--^ ^- ^ ^
///    rank 8 to rank 1, '/'-separated   |   |   |   | fullmove
///                                side  castling |   | halfmove clock
///                                                en-passant target ('-' = none)
///
/// We commit to *ASCII-only* FEN in both directions — input and output.
/// Unicode figurines (♔♕♖♗♘♙ …) are an opt-in display format handled by
/// the CLI renderer; they never cross the FEN boundary.
///
/// Parsing is split by design:
///   * `serialize_fen` is polymorphic — it walks the abstract `Board`
///     interface, so it works unchanged against a future bitboard
///     implementation.
///   * Parsing produces the concrete `Board8x8Mailbox` and therefore
///     lives as a static factory on that class (see
///     `board8x8_mailbox.hpp`). If/when other concrete boards appear,
///     each provides its own `from_fen`.
///
/// References:
///   * https://www.chessprogramming.org/Forsyth-Edwards_Notation
#pragma once

#include <string>
#include <string_view>

namespace chesserazade {

class Board;

/// Recoverable error from FEN parsing. Carries a human-readable
/// message so the CLI can show it to the user verbatim.
struct FenError {
    std::string message;
};

/// FEN for the standard chess starting position.
inline constexpr std::string_view STARTING_POSITION_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

/// Serialize any `Board` implementation to FEN. The output uses the
/// canonical castling-rights order "KQkq" and emits '-' where a field
/// is empty. Output is always ASCII.
[[nodiscard]] std::string serialize_fen(const Board& b);

} // namespace chesserazade

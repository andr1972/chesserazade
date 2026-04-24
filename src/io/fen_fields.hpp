// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Intermediate representation produced by the FEN parser.
///
/// Both concrete `Board` implementations (`Board8x8Mailbox`,
/// `BoardBitboard`) need the same parse logic for FEN but
/// populate different internal structures. Rather than
/// template the parser or wire setters through the abstract
/// `Board` interface (FEN setters don't belong on the public
/// API), we run the parser once into this plain struct and let
/// each implementation's `from_fen` copy the fields into its
/// native representation.
///
/// This header is private to `src/` — callers use the
/// public `Board8x8Mailbox::from_fen` / `BoardBitboard::from_fen`
/// entry points, not `parse_fen_fields` directly.
#pragma once

#include <chesserazade/board.hpp>
#include <chesserazade/fen.hpp>
#include <chesserazade/types.hpp>

#include <array>
#include <expected>
#include <string_view>

namespace chesserazade {

struct FenFields {
    /// Pieces indexed by `Square` under LERF. `Piece::none()`
    /// for empty squares.
    std::array<Piece, NUM_SQUARES> squares{};

    Color side = Color::White;
    CastlingRights castling{};
    Square ep = Square::None;
    int halfmove = 0;
    int fullmove = 1;
};

[[nodiscard]] std::expected<FenFields, FenError>
parse_fen_fields(std::string_view fen);

} // namespace chesserazade

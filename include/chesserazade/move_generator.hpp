// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Move generator — pseudo-legal and legal move enumeration.
///
/// The classical move-generation pipeline has two stages:
///
///   1. **Pseudo-legal generation** — enumerate every move that a piece
///      can physically make (correct piece-type movement, empty or
///      enemy target squares, castling squares free) WITHOUT checking
///      whether the resulting position leaves the moving side's king in
///      check. This stage is fast and simple.
///
///   2. **Legality filtering** — apply each pseudo-legal move with
///      `make_move`, test whether the moving side's king is in check,
///      and `unmake_move`. Only moves that pass survive.
///
/// Callers in the search loop should use `generate_legal`, which hides
/// the two stages. `generate_pseudo_legal` and `is_in_check` are public
/// for testing and for the 0.7+ `Search` (which may apply its own
/// filtering strategy).
///
/// See https://www.chessprogramming.org/Move_Generation for a survey
/// of generation strategies. This implementation is the simplest
/// "iterate squares, classify piece, enumerate targets" approach, which
/// is correct and readable at the cost of being slower than a bitboard
/// or pre-computed attack-table approach (that arrives in 1.1).
#pragma once

#include <chesserazade/board.hpp>
#include <chesserazade/move.hpp>

#include <array>
#include <cstddef>

namespace chesserazade {

/// Fixed-capacity move list. 256 slots give comfortable headroom over
/// the theoretical maximum of 218 legal moves in any reachable position.
/// See https://www.chessprogramming.org/Chess_Position
struct MoveList {
    static constexpr std::size_t CAPACITY = 256;

    std::array<Move, CAPACITY> moves{};
    std::size_t count = 0;

    void push(const Move& m) noexcept { moves[count++] = m; }
    [[nodiscard]] Move* begin() noexcept { return moves.data(); }
    [[nodiscard]] Move* end() noexcept { return moves.data() + count; }
    [[nodiscard]] const Move* begin() const noexcept { return moves.data(); }
    [[nodiscard]] const Move* end() const noexcept { return moves.data() + count; }
    [[nodiscard]] bool empty() const noexcept { return count == 0; }
};

/// Stateless move generator that works against the abstract
/// `Board` interface. Behind the scenes, when the concrete type
/// is a `BoardBitboard`, the entry points dispatch to the
/// faster `BitboardMoveGenerator`; for `Board8x8Mailbox` they
/// use the classical square-walking implementation.
class MoveGenerator {
public:
    MoveGenerator() = delete;

    /// Generate all pseudo-legal moves for the side to move.
    [[nodiscard]] static MoveList generate_pseudo_legal(const Board& b);

    /// Generate all legal moves for the side to move.
    ///
    /// Internally applies `make_move` / `unmake_move` for each
    /// pseudo-legal candidate and retains only those that do not leave
    /// the moving side's king in check. `b` must be non-const because
    /// the legality filter mutates (and immediately restores) it.
    [[nodiscard]] static MoveList generate_legal(Board& b);

    /// True if `side`'s king is attacked by any piece of the other color.
    [[nodiscard]] static bool is_in_check(const Board& b, Color side) noexcept;

    /// True if square `sq` is attacked by any piece belonging to
    /// `attacker_color`. Uses the "look from the target" pattern:
    /// rays and jump patterns are cast outward from `sq`; if the first
    /// piece found along a ray (or at a knight/pawn offset) is the
    /// matching enemy piece type, `sq` is attacked.
    [[nodiscard]] static bool is_square_attacked(const Board& b, Square sq,
                                                 Color attacker_color) noexcept;
};

class BoardBitboard;

/// Bitboard-native move generator. Same interface shape as
/// `MoveGenerator`, but takes `BoardBitboard` directly so it
/// can read the piece bitboards, union them, mask with
/// per-color occupancy, and serialize move sets via `pop_lsb` —
/// all in constant time per piece rather than the O(64) walk
/// the abstract-interface generator pays.
class BitboardMoveGenerator {
public:
    BitboardMoveGenerator() = delete;

    [[nodiscard]] static MoveList generate_pseudo_legal(const BoardBitboard& b);
    [[nodiscard]] static MoveList generate_legal(BoardBitboard& b);
    [[nodiscard]] static bool is_in_check(const BoardBitboard& b, Color side) noexcept;
    [[nodiscard]] static bool is_square_attacked(const BoardBitboard& b,
                                                 Square sq,
                                                 Color attacker_color) noexcept;
};

} // namespace chesserazade

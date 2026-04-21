/// Bitboard — 64-bit set of squares, one bit per square.
///
/// A chess board has exactly 64 squares, so a `std::uint64_t`
/// can represent any *set* of squares: "all pieces", "all
/// squares attacked by this knight", "all squares on the
/// e-file". Two such sets combine with bitwise operations —
/// `union = a | b`, `intersection = a & b`, `difference = a & ~b`
/// — which a 64-bit CPU executes in one instruction. This is
/// why the world's fastest engines are all bitboard engines.
///
/// We index under **little-endian rank-file mapping** (the same
/// LERF convention used by `Square`): bit 0 is a1, bit 7 is h1,
/// bit 56 is a8, bit 63 is h8. One consequence: shifting a
/// bitboard left by 8 advances every piece one rank towards
/// white's back row becoming black's; shifting right by 8
/// retreats one rank.
///
/// See https://www.chessprogramming.org/Bitboards for the full
/// introduction. This module covers:
///   * the `Bitboard` type and its square / bit helpers;
///   * `popcount` / `lsb` / `msb` / `pop_lsb` — the four bit
///     operations every bitboard algorithm uses;
///   * file and rank constant masks;
///   * precomputed attack tables for the non-sliding pieces
///     (king, knight, pawn).
///
/// Slider attacks (rook / bishop / queen) arrive in 1.1.3.
#pragma once

#include <chesserazade/types.hpp>

#include <bit>
#include <cstdint>

namespace chesserazade {

/// A set of squares encoded as a 64-bit bitmask. Bit `i` set
/// means the square with index `i` (under LERF) is in the set.
using Bitboard = std::uint64_t;

// ---------------------------------------------------------------------------
// Single-square conversions
// ---------------------------------------------------------------------------

/// The single-bit bitboard corresponding to `s`. A valid
/// `Square` (A1..H8) always produces a bitboard with exactly
/// one bit set.
[[nodiscard]] constexpr Bitboard bb_of(Square s) noexcept {
    return Bitboard{1} << to_index(s);
}

/// True if `s` is in the set `b`.
[[nodiscard]] constexpr bool contains(Bitboard b, Square s) noexcept {
    return (b & bb_of(s)) != 0;
}

/// `b` with `s` added.
[[nodiscard]] constexpr Bitboard set(Bitboard b, Square s) noexcept {
    return b | bb_of(s);
}

/// `b` with `s` removed.
[[nodiscard]] constexpr Bitboard clear(Bitboard b, Square s) noexcept {
    return b & ~bb_of(s);
}

// ---------------------------------------------------------------------------
// Bit operations
// ---------------------------------------------------------------------------
//
// All four of these map to a single CPU instruction on modern
// x86_64 (POPCNT + BMI1's BLSR), which is the whole point of
// using bitboards. They live as thin wrappers in <bit>; we
// re-export them here so bitboard call sites read naturally.

/// Number of bits set — the cardinality of the square set.
[[nodiscard]] constexpr int popcount(Bitboard b) noexcept {
    return std::popcount(b);
}

/// Least-significant set bit as a `Square`. Precondition:
/// `b != 0`. Returns the square with the smallest LERF index
/// present in `b`.
[[nodiscard]] constexpr Square lsb(Bitboard b) noexcept {
    return static_cast<Square>(std::countr_zero(b));
}

/// Most-significant set bit as a `Square`. Precondition:
/// `b != 0`.
[[nodiscard]] constexpr Square msb(Bitboard b) noexcept {
    return static_cast<Square>(63 - std::countl_zero(b));
}

/// Extract and clear the least-significant set bit. Standard
/// idiom for iterating a bitboard's squares:
/// `while (b) { Square s = pop_lsb(b); ... }`.
[[nodiscard]] constexpr Square pop_lsb(Bitboard& b) noexcept {
    const Square s = lsb(b);
    b &= b - 1; // clear the LSB
    return s;
}

// ---------------------------------------------------------------------------
// File / rank masks
// ---------------------------------------------------------------------------

constexpr Bitboard FILE_A = 0x0101010101010101ULL;
constexpr Bitboard FILE_B = FILE_A << 1;
constexpr Bitboard FILE_C = FILE_A << 2;
constexpr Bitboard FILE_D = FILE_A << 3;
constexpr Bitboard FILE_E = FILE_A << 4;
constexpr Bitboard FILE_F = FILE_A << 5;
constexpr Bitboard FILE_G = FILE_A << 6;
constexpr Bitboard FILE_H = FILE_A << 7;

constexpr Bitboard RANK_1 = 0xFFULL;
constexpr Bitboard RANK_2 = RANK_1 << 8;
constexpr Bitboard RANK_3 = RANK_1 << 16;
constexpr Bitboard RANK_4 = RANK_1 << 24;
constexpr Bitboard RANK_5 = RANK_1 << 32;
constexpr Bitboard RANK_6 = RANK_1 << 40;
constexpr Bitboard RANK_7 = RANK_1 << 48;
constexpr Bitboard RANK_8 = RANK_1 << 56;

constexpr Bitboard ALL_SQUARES = ~Bitboard{0};
constexpr Bitboard NO_SQUARES  = Bitboard{0};

// ---------------------------------------------------------------------------
// Attack tables for non-sliding pieces
// ---------------------------------------------------------------------------
//
// Precomputed at program start from a fixed algorithm (no random
// data involved). Indexed by `Square`; pawn tables additionally
// by `Color`. A lookup is O(1) — a single memory load.
//
// The slider attacks (`rook`, `bishop`, `queen`) are more
// involved because they depend on the occupancy of the rest of
// the board, and ship in 1.1.3.

class Attacks {
public:
    /// Every square a king on `sq` attacks (i.e. the 3×3 box
    /// around `sq`, minus `sq` itself, clipped to the board).
    [[nodiscard]] static Bitboard king(Square sq) noexcept;

    /// Every square a knight on `sq` attacks — up to 8 L-shape
    /// jumps, clipped to the board.
    [[nodiscard]] static Bitboard knight(Square sq) noexcept;

    /// Every square a pawn of color `c` on `sq` attacks
    /// diagonally. Two squares max; one near a-/h-file, zero
    /// when a pawn is somehow on rank 1 or 8 (malformed
    /// position — the table still returns the correct empty /
    /// clipped mask).
    [[nodiscard]] static Bitboard pawn(Color c, Square sq) noexcept;

    Attacks() = delete;
};

} // namespace chesserazade

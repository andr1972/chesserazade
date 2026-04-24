// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Attack-table initialization for non-sliding pieces.
///
/// We build the tables by walking every square and computing
/// the set of destinations by explicit rank / file coordinates
/// — the same "integer bounds-check" idiom the mailbox move
/// generator uses. Correctness-first; a table built in a
/// microsecond once per program startup is nothing compared to
/// the millions of lookups during search.

#include <chesserazade/bitboard.hpp>

#include <array>
#include <cstddef>

namespace chesserazade {

namespace {

struct Tables {
    std::array<Bitboard, NUM_SQUARES> king{};
    std::array<Bitboard, NUM_SQUARES> knight{};
    /// [color][square] — white = 0, black = 1.
    std::array<std::array<Bitboard, NUM_SQUARES>, 2> pawn{};

    Tables() {
        // King: all 8 deltas of magnitude 1 in either axis,
        // bounds-checked. Compute per square.
        constexpr int king_deltas[8][2] = {
            {+1, 0}, {-1, 0}, {0, +1}, {0, -1},
            {+1, +1}, {+1, -1}, {-1, +1}, {-1, -1},
        };
        constexpr int knight_deltas[8][2] = {
            {+2, +1}, {+2, -1}, {-2, +1}, {-2, -1},
            {+1, +2}, {+1, -2}, {-1, +2}, {-1, -2},
        };

        for (int i = 0; i < 64; ++i) {
            const int r = i / 8;
            const int f = i % 8;

            Bitboard k = 0;
            for (const auto& d : king_deltas) {
                const int nr = r + d[0];
                const int nf = f + d[1];
                if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                    k |= Bitboard{1} << (nr * 8 + nf);
                }
            }
            king[static_cast<std::size_t>(i)] = k;

            Bitboard n = 0;
            for (const auto& d : knight_deltas) {
                const int nr = r + d[0];
                const int nf = f + d[1];
                if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                    n |= Bitboard{1} << (nr * 8 + nf);
                }
            }
            knight[static_cast<std::size_t>(i)] = n;

            // Pawn attacks: a pawn of color C on square s
            // attacks the two diagonally-forward squares in
            // the direction C moves. White moves up (+rank),
            // black moves down.
            Bitboard pw = 0;
            Bitboard pb = 0;
            // White pawn attacks — diagonals to +rank.
            if (r + 1 < 8) {
                if (f - 1 >= 0) pw |= Bitboard{1} << ((r + 1) * 8 + (f - 1));
                if (f + 1 < 8)  pw |= Bitboard{1} << ((r + 1) * 8 + (f + 1));
            }
            // Black pawn attacks — diagonals to -rank.
            if (r - 1 >= 0) {
                if (f - 1 >= 0) pb |= Bitboard{1} << ((r - 1) * 8 + (f - 1));
                if (f + 1 < 8)  pb |= Bitboard{1} << ((r - 1) * 8 + (f + 1));
            }
            pawn[0][static_cast<std::size_t>(i)] = pw;
            pawn[1][static_cast<std::size_t>(i)] = pb;
        }
    }
};

const Tables& tables() noexcept {
    static const Tables t;
    return t;
}

} // namespace

Bitboard Attacks::king(Square sq) noexcept {
    return tables().king[to_index(sq)];
}

Bitboard Attacks::knight(Square sq) noexcept {
    return tables().knight[to_index(sq)];
}

Bitboard Attacks::pawn(Color c, Square sq) noexcept {
    return tables().pawn[static_cast<std::size_t>(c)][to_index(sq)];
}

// ---------------------------------------------------------------------------
// Slider attacks — classical loop-based ray walk
// ---------------------------------------------------------------------------
//
// Each call walks up to 7 squares per ray, stopping at the
// first occupied square. The first blocker is included in the
// returned attack set (a blocker that belongs to the enemy is
// a legal capture target; a friendly blocker is filtered out
// higher up by masking with `~our_occupancy`).
//
// This is O(distance-to-blocker), not O(1). Magic bitboards
// would make it O(1) but add hundreds of lines of setup; the
// educational value of the O(1) upgrade is marginal and we
// treat it as a post-1.1 optimisation (1.1.5). The loop-based
// version is already dramatically faster than the mailbox
// square-by-square generator because it processes whole rays
// at once and interacts with the rest of the generator through
// bitwise-AND with our/their occupancy — one instruction each.

namespace {

/// Walk from `sq` in direction (dr, df), returning the bitboard
/// of every square touched up to and including the first
/// blocker, or to the board edge.
[[nodiscard]] Bitboard ray(Square sq, int dr, int df,
                           Bitboard occ) noexcept {
    Bitboard result = 0;
    int r = static_cast<int>(rank_of(sq)) + dr;
    int f = static_cast<int>(file_of(sq)) + df;
    while (r >= 0 && r < 8 && f >= 0 && f < 8) {
        const Bitboard mask = Bitboard{1} << (r * 8 + f);
        result |= mask;
        if (occ & mask) break;
        r += dr;
        f += df;
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Reference (loop-based) slider attacks — always available as
// the "if anything goes wrong, we can still compute this"
// baseline. The public `Attacks::rook/bishop` call through a
// function pointer so that magic-bitboards / PEXT init paths
// can swap in faster implementations at program startup.
// ---------------------------------------------------------------------------

Bitboard Attacks::rook_loop(Square sq, Bitboard occ) noexcept {
    return ray(sq, +1,  0, occ)   // N
         | ray(sq, -1,  0, occ)   // S
         | ray(sq,  0, +1, occ)   // E
         | ray(sq,  0, -1, occ);  // W
}

Bitboard Attacks::bishop_loop(Square sq, Bitboard occ) noexcept {
    return ray(sq, +1, +1, occ)   // NE
         | ray(sq, +1, -1, occ)   // NW
         | ray(sq, -1, +1, occ)   // SE
         | ray(sq, -1, -1, occ);  // SW
}

namespace {
Attacks::SliderFn g_rook_attack   = &Attacks::rook_loop;
Attacks::SliderFn g_bishop_attack = &Attacks::bishop_loop;
} // namespace

Bitboard Attacks::rook(Square sq, Bitboard occ) noexcept {
    return g_rook_attack(sq, occ);
}

Bitboard Attacks::bishop(Square sq, Bitboard occ) noexcept {
    return g_bishop_attack(sq, occ);
}

Bitboard Attacks::queen(Square sq, Bitboard occ) noexcept {
    return g_rook_attack(sq, occ) | g_bishop_attack(sq, occ);
}

void Attacks::set_rook_attack_fn(SliderFn fn) noexcept {
    g_rook_attack = fn;
}
void Attacks::set_bishop_attack_fn(SliderFn fn) noexcept {
    g_bishop_attack = fn;
}
Attacks::SliderFn Attacks::rook_fn() noexcept { return g_rook_attack; }
Attacks::SliderFn Attacks::bishop_fn() noexcept { return g_bishop_attack; }

} // namespace chesserazade

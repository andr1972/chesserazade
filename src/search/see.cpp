// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
#include "search/see.hpp"

#include "board/board_bitboard.hpp"

#include <chesserazade/bitboard.hpp>
#include <chesserazade/evaluator.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace chesserazade {

namespace {

/// Bitboard of all pieces (any colour) attacking `sq` given the
/// occupancy `occ`. Slider attacks are recomputed against `occ` so
/// we can pass a thinned occupancy after a piece is removed and
/// pick up the X-rays it was blocking.
[[nodiscard]] Bitboard attackers_to(const BoardBitboard& b, Square sq,
                                    Bitboard occ) noexcept {
    Bitboard a = 0;
    a |= Attacks::pawn(Color::Black, sq) & b.pieces(Color::White, PieceType::Pawn);
    a |= Attacks::pawn(Color::White, sq) & b.pieces(Color::Black, PieceType::Pawn);
    a |= Attacks::knight(sq)
         & (b.pieces(Color::White, PieceType::Knight)
            | b.pieces(Color::Black, PieceType::Knight));
    a |= Attacks::king(sq)
         & (b.pieces(Color::White, PieceType::King)
            | b.pieces(Color::Black, PieceType::King));
    const Bitboard rook_queens =
        b.pieces(Color::White, PieceType::Rook)
        | b.pieces(Color::Black, PieceType::Rook)
        | b.pieces(Color::White, PieceType::Queen)
        | b.pieces(Color::Black, PieceType::Queen);
    a |= Attacks::rook(sq, occ) & rook_queens;
    const Bitboard bishop_queens =
        b.pieces(Color::White, PieceType::Bishop)
        | b.pieces(Color::Black, PieceType::Bishop)
        | b.pieces(Color::White, PieceType::Queen)
        | b.pieces(Color::Black, PieceType::Queen);
    a |= Attacks::bishop(sq, occ) & bishop_queens;
    return a & occ;
}

/// Among `attackers`, find the cheapest piece of `side` and return
/// its bitboard (single-bit) and type. Returns {0, None} if `side`
/// has no attackers in the set.
struct SmallestAttacker {
    Bitboard bit;     // single-bit bitboard of the chosen attacker
    PieceType type;   // PieceType::None when nothing left
};

[[nodiscard]] SmallestAttacker
pick_smallest(const BoardBitboard& b, Color side,
              Bitboard attackers) noexcept {
    static constexpr std::array<PieceType, 6> ORDER = {
        PieceType::Pawn,   PieceType::Knight, PieceType::Bishop,
        PieceType::Rook,   PieceType::Queen,  PieceType::King,
    };
    for (PieceType pt : ORDER) {
        const Bitboard bb = attackers & b.pieces(side, pt);
        if (bb != 0) {
            return {bb & (~bb + 1), pt};  // isolate lsb
        }
    }
    return {0, PieceType::None};
}

} // namespace

int see(const BoardBitboard& b, const Move& m) noexcept {
    // Initial gain = the piece we just captured. En-passant takes a
    // pawn regardless of what `m.captured_piece` records.
    const PieceType victim_type =
        (m.kind == MoveKind::EnPassant) ? PieceType::Pawn
                                        : m.captured_piece.type;
    std::array<int, 32> gain{};
    std::size_t d = 0;
    gain[d] = piece_value(victim_type);

    // Remove the first attacker from the occupancy so the next
    // iteration's slider attacks see through it (X-ray detection).
    Bitboard occ = b.occupancy() ^ (Bitboard{1} << to_index(m.from));
    Bitboard attackers = attackers_to(b, m.to, occ) & occ;

    // The next side to recapture is the opponent of the original
    // mover (we just played `m`).
    Color side = (b.side_to_move() == Color::White)
        ? Color::Black : Color::White;
    PieceType last_attacker = m.moved_piece.type;

    while (true) {
        ++d;
        gain[d] = piece_value(last_attacker) - gain[d - 1];
        // Pruning: if the running maximum has already gone below 0,
        // the side-to-move would refuse to continue the exchange.
        if (std::max(-gain[d - 1], gain[d]) < 0) {
            break;
        }
        const SmallestAttacker pick = pick_smallest(b, side, attackers);
        if (pick.type == PieceType::None || d + 1 >= gain.size()) {
            break;
        }
        // Remove the chosen attacker from occupancy + the running
        // attackers set; recompute X-ray attackers from any newly-
        // exposed sliders.
        occ ^= pick.bit;
        attackers ^= pick.bit;
        if (pick.type == PieceType::Pawn
            || pick.type == PieceType::Bishop
            || pick.type == PieceType::Queen) {
            const Bitboard diag =
                b.pieces(Color::White, PieceType::Bishop)
                | b.pieces(Color::Black, PieceType::Bishop)
                | b.pieces(Color::White, PieceType::Queen)
                | b.pieces(Color::Black, PieceType::Queen);
            attackers |= Attacks::bishop(m.to, occ) & diag;
        }
        if (pick.type == PieceType::Rook
            || pick.type == PieceType::Queen) {
            const Bitboard ortho =
                b.pieces(Color::White, PieceType::Rook)
                | b.pieces(Color::Black, PieceType::Rook)
                | b.pieces(Color::White, PieceType::Queen)
                | b.pieces(Color::Black, PieceType::Queen);
            attackers |= Attacks::rook(m.to, occ) & ortho;
        }
        attackers &= occ;
        last_attacker = pick.type;
        side = (side == Color::White) ? Color::Black : Color::White;
    }

    // Backward pass: each side, given the future, chooses to stop
    // the exchange before going deeper if doing so is better.
    while (--d > 0) {
        gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
    }
    return gain[0];
}

} // namespace chesserazade

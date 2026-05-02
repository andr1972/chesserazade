// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Implementation of the Simplified Evaluation Function.
///
/// Tables are laid out in **LERF order** to match the rest of the
/// codebase: index 0 = a1, index 7 = h1, index 56 = a8, index 63 =
/// h8. They read like a board printed from white's perspective —
/// rank 1 at the top of the array, rank 8 at the bottom.
///
/// This orientation is opposite to how most chess programming
/// articles print their PSTs (they usually show rank 8 at top).
/// The numeric values below are the same; only the visual order
/// differs.  See the `rank N` comments on each row.

#include <chesserazade/evaluator.hpp>

#include "board/board_bitboard.hpp"

#include <chesserazade/bitboard.hpp>
#include <chesserazade/board.hpp>
#include <chesserazade/types.hpp>

#include <array>
#include <cstdint>

namespace chesserazade {

namespace {

using PstTable = std::array<int, NUM_SQUARES>;

// ---------------------------------------------------------------------------
// Piece-square tables (white's perspective, LERF-indexed)
// ---------------------------------------------------------------------------

constexpr PstTable PAWN_PST = {
//   a   b   c   d   e   f   g   h
     0,  0,  0,  0,  0,  0,  0,  0,   // rank 1
     5, 10, 10,-20,-20, 10, 10,  5,   // rank 2
     5, -5,-10,  0,  0,-10, -5,  5,   // rank 3
     0,  0,  0, 20, 20,  0,  0,  0,   // rank 4
     5,  5, 10, 25, 25, 10,  5,  5,   // rank 5
    10, 10, 20, 30, 30, 20, 10, 10,   // rank 6
    50, 50, 50, 50, 50, 50, 50, 50,   // rank 7
     0,  0,  0,  0,  0,  0,  0,  0,   // rank 8
};

constexpr PstTable KNIGHT_PST = {
   -50,-40,-30,-30,-30,-30,-40,-50,   // rank 1
   -40,-20,  0,  5,  5,  0,-20,-40,   // rank 2
   -30,  5, 10, 15, 15, 10,  5,-30,   // rank 3
   -30,  0, 15, 20, 20, 15,  0,-30,   // rank 4
   -30,  5, 15, 20, 20, 15,  5,-30,   // rank 5
   -30,  0, 10, 15, 15, 10,  0,-30,   // rank 6
   -40,-20,  0,  0,  0,  0,-20,-40,   // rank 7
   -50,-40,-30,-30,-30,-30,-40,-50,   // rank 8
};

constexpr PstTable BISHOP_PST = {
   -20,-10,-10,-10,-10,-10,-10,-20,   // rank 1
   -10,  5,  0,  0,  0,  0,  5,-10,   // rank 2
   -10, 10, 10, 10, 10, 10, 10,-10,   // rank 3
   -10,  0, 10, 10, 10, 10,  0,-10,   // rank 4
   -10,  5,  5, 10, 10,  5,  5,-10,   // rank 5
   -10,  0,  5, 10, 10,  5,  0,-10,   // rank 6
   -10,  0,  0,  0,  0,  0,  0,-10,   // rank 7
   -20,-10,-10,-10,-10,-10,-10,-20,   // rank 8
};

constexpr PstTable ROOK_PST = {
     0,  0,  0,  5,  5,  0,  0,  0,   // rank 1 — central files mild bonus
    -5,  0,  0,  0,  0,  0,  0, -5,   // rank 2
    -5,  0,  0,  0,  0,  0,  0, -5,   // rank 3
    -5,  0,  0,  0,  0,  0,  0, -5,   // rank 4
    -5,  0,  0,  0,  0,  0,  0, -5,   // rank 5
    -5,  0,  0,  0,  0,  0,  0, -5,   // rank 6
     5, 10, 10, 10, 10, 10, 10,  5,   // rank 7 — classical 7th-rank bonus
     0,  0,  0,  0,  0,  0,  0,  0,   // rank 8
};

constexpr PstTable QUEEN_PST = {
   -20,-10,-10, -5, -5,-10,-10,-20,   // rank 1
   -10,  0,  0,  0,  0,  0,  0,-10,   // rank 2
   -10,  0,  5,  5,  5,  5,  0,-10,   // rank 3
    -5,  0,  5,  5,  5,  5,  0, -5,   // rank 4
     0,  0,  5,  5,  5,  5,  0, -5,   // rank 5
   -10,  5,  5,  5,  5,  5,  0,-10,   // rank 6
   -10,  0,  5,  0,  0,  0,  0,-10,   // rank 7
   -20,-10,-10, -5, -5,-10,-10,-20,   // rank 8
};

/// King middlegame table. An endgame variant (tucked-in becomes
/// worse than centralized) is a 0.6+ concern; for 0.5 we stay
/// with the single middlegame orientation so the evaluator is
/// phase-independent.
constexpr PstTable KING_PST = {
    20, 30, 10,  0,  0, 10, 30, 20,   // rank 1 — castled positions
    20, 20,  0,  0,  0,  0, 20, 20,   // rank 2 — pawn shelter
   -10,-20,-20,-20,-20,-20,-20,-10,   // rank 3
   -20,-30,-30,-40,-40,-30,-30,-20,   // rank 4
   -30,-40,-40,-50,-50,-40,-40,-30,   // rank 5
   -30,-40,-40,-50,-50,-40,-40,-30,   // rank 6
   -30,-40,-40,-50,-50,-40,-40,-30,   // rank 7
   -30,-40,-40,-50,-50,-40,-40,-30,   // rank 8
};

/// Return the table for a given piece type. Empty squares and
/// sentinels resolve to an all-zero fallback table so the caller
/// does not have to branch; in practice `evaluate` skips empty
/// squares before the lookup.
[[nodiscard]] const PstTable& pst_for(PieceType pt) noexcept {
    switch (pt) {
        case PieceType::Pawn:   return PAWN_PST;
        case PieceType::Knight: return KNIGHT_PST;
        case PieceType::Bishop: return BISHOP_PST;
        case PieceType::Rook:   return ROOK_PST;
        case PieceType::Queen:  return QUEEN_PST;
        case PieceType::King:   return KING_PST;
        case PieceType::None:   break;
    }
    static constexpr PstTable zeros{};
    return zeros;
}

/// Mirror a LERF square index across the horizontal midline so
/// that a table laid out for white reads correctly for black. The
/// XOR-56 trick flips only the rank (upper 3 bits) and preserves
/// the file (lower 3 bits). See
/// https://www.chessprogramming.org/Piece-Square_Tables
[[nodiscard]] constexpr std::uint8_t mirror(std::uint8_t sq) noexcept {
    return static_cast<std::uint8_t>(sq ^ 56u);
}

} // namespace

int evaluate(const Board& board) noexcept {
    int score = 0;
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Piece p = board.piece_at(static_cast<Square>(i));
        if (p.is_none()) continue;

        const int material = piece_value(p.type);
        const std::uint8_t idx = (p.color == Color::White) ? i : mirror(i);
        const int positional = pst_for(p.type)[idx];
        const int total = material + positional;

        score += (p.color == Color::White) ? total : -total;
    }
    return (board.side_to_move() == Color::White) ? score : -score;
}

int piece_contribution(Piece p, Square sq) noexcept {
    if (p.is_none()) return 0;
    const std::uint8_t i = static_cast<std::uint8_t>(sq);
    const int material = piece_value(p.type);
    const std::uint8_t idx = (p.color == Color::White) ? i : mirror(i);
    const int positional = pst_for(p.type)[idx];
    const int total = material + positional;
    return (p.color == Color::White) ? total : -total;
}

int psqt_delta(Piece p, Square from, Square to) noexcept {
    if (p.is_none()) return 0;
    const PstTable& tbl = pst_for(p.type);
    const std::uint8_t fi = static_cast<std::uint8_t>(from);
    const std::uint8_t ti = static_cast<std::uint8_t>(to);
    const std::uint8_t f_idx = (p.color == Color::White) ? fi : mirror(fi);
    const std::uint8_t t_idx = (p.color == Color::White) ? ti : mirror(ti);
    const int delta = tbl[t_idx] - tbl[f_idx];
    return (p.color == Color::White) ? delta : -delta;
}

namespace {

/// Per-piece-type weight for king-ring attacks. SF-classical-ish
/// numbers, flat (no mg/eg curve since chesserazade has no phase
/// blending yet). Knight = 80 because a knight near the king is a
/// classical mating-net builder; bishop / rook smaller because
/// their long-range attacks on the ring are less immediate; queen
/// matches knight weight because of its versatility.
constexpr int KING_ATTACKER_KNIGHT = 80;
constexpr int KING_ATTACKER_BISHOP = 50;
constexpr int KING_ATTACKER_ROOK   = 40;
constexpr int KING_ATTACKER_QUEEN  = 80;

[[nodiscard]] int king_danger_from(const BoardBitboard& b,
                                    Color attacker) noexcept {
    const Color defender = (attacker == Color::White) ? Color::Black
                                                       : Color::White;
    const Bitboard def_king = b.pieces(defender, PieceType::King);
    if (def_king == 0) return 0;
    const Square king_sq = lsb(def_king);
    const Bitboard king_ring = Attacks::king(king_sq);
    const Bitboard occ = b.occupancy();

    int attack_value = 0;
    int attackers_count = 0;

    Bitboard knights = b.pieces(attacker, PieceType::Knight);
    while (knights != 0) {
        const Square sq = pop_lsb(knights);
        if ((Attacks::knight(sq) & king_ring) != 0) {
            ++attackers_count;
            attack_value += KING_ATTACKER_KNIGHT;
        }
    }
    Bitboard bishops = b.pieces(attacker, PieceType::Bishop);
    while (bishops != 0) {
        const Square sq = pop_lsb(bishops);
        if ((Attacks::bishop(sq, occ) & king_ring) != 0) {
            ++attackers_count;
            attack_value += KING_ATTACKER_BISHOP;
        }
    }
    Bitboard rooks = b.pieces(attacker, PieceType::Rook);
    while (rooks != 0) {
        const Square sq = pop_lsb(rooks);
        if ((Attacks::rook(sq, occ) & king_ring) != 0) {
            ++attackers_count;
            attack_value += KING_ATTACKER_ROOK;
        }
    }
    Bitboard queens = b.pieces(attacker, PieceType::Queen);
    while (queens != 0) {
        const Square sq = pop_lsb(queens);
        const Bitboard q_atk =
            Attacks::bishop(sq, occ) | Attacks::rook(sq, occ);
        if ((q_atk & king_ring) != 0) {
            ++attackers_count;
            attack_value += KING_ATTACKER_QUEEN;
        }
    }

    // A single distant attacker isn't dangerous — the threshold
    // requires ≥ 2 to fire. Scale the danger by the attacker count
    // so '4 attackers' is twice as dangerous as '2 attackers' at the
    // same per-piece weight average.
    if (attackers_count < 2) return 0;
    return (attack_value * attackers_count) / 4;
}

} // namespace

int king_safety_stm(const BoardBitboard& b) noexcept {
    const int white_threat = king_danger_from(b, Color::White);
    const int black_threat = king_danger_from(b, Color::Black);
    const int diff = white_threat - black_threat;
    return (b.side_to_move() == Color::White) ? diff : -diff;
}

} // namespace chesserazade

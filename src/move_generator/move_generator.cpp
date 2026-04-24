// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Move generator implementation.
///
/// Approach: iterate all 64 squares, find pieces of the side-to-move
/// color, and dispatch to a per-piece-type helper that appends moves to
/// the MoveList. All helpers operate on `const Board&` (read-only);
/// only `generate_legal` needs mutable access (for make/unmake).
///
/// Coordinate arithmetic uses explicit rank/file integers rather than
/// raw square offsets to avoid wrap-around bugs (e.g. a rook on H1
/// "moving east" would land on A2 under naive +1 arithmetic).

#include <chesserazade/move_generator.hpp>

#include "board/board_bitboard.hpp"

#include <cassert>
#include <cstdint>

namespace chesserazade {

namespace {

// ---------------------------------------------------------------------------
// Direction tables
// ---------------------------------------------------------------------------

struct Delta {
    int dr; // rank delta
    int df; // file delta
};

constexpr Delta ROOK_DIRS[4] = {{+1, 0}, {-1, 0}, {0, +1}, {0, -1}};
constexpr Delta BISHOP_DIRS[4] = {{+1, +1}, {+1, -1}, {-1, +1}, {-1, -1}};
constexpr Delta KNIGHT_JUMPS[8] = {
    {+2, +1}, {+2, -1}, {-2, +1}, {-2, -1},
    {+1, +2}, {+1, -2}, {-1, +2}, {-1, -2},
};
constexpr Delta KING_DIRS[8] = {
    {+1, 0}, {-1, 0}, {0, +1}, {0, -1},
    {+1, +1}, {+1, -1}, {-1, +1}, {-1, -1},
};

// ---------------------------------------------------------------------------
// Square helpers
// ---------------------------------------------------------------------------

[[nodiscard]] inline int sq_rank(Square s) noexcept {
    return static_cast<int>(rank_of(s));
}
[[nodiscard]] inline int sq_file(Square s) noexcept {
    return static_cast<int>(file_of(s));
}
[[nodiscard]] inline Square make_sq(int r, int f) noexcept {
    return make_square(static_cast<File>(f), static_cast<Rank>(r));
}
[[nodiscard]] inline bool in_bounds(int r, int f) noexcept {
    return r >= 0 && r <= 7 && f >= 0 && f <= 7;
}

// ---------------------------------------------------------------------------
// Move builders
// ---------------------------------------------------------------------------

[[nodiscard]] Move quiet(Square from, Square to, Piece moved) noexcept {
    Move m;
    m.from = from;
    m.to = to;
    m.kind = MoveKind::Quiet;
    m.moved_piece = moved;
    return m;
}

[[nodiscard]] Move capture(Square from, Square to, Piece moved, Piece captured) noexcept {
    Move m;
    m.from = from;
    m.to = to;
    m.kind = MoveKind::Capture;
    m.moved_piece = moved;
    m.captured_piece = captured;
    return m;
}

// Append all four promotion variants to `ml`.
void push_promotions(MoveList& ml, Square from, Square to,
                     Piece pawn, Piece captured, bool is_capture) noexcept {
    const MoveKind kind = is_capture ? MoveKind::PromotionCapture : MoveKind::Promotion;
    for (PieceType pt : {PieceType::Queen, PieceType::Rook,
                         PieceType::Bishop, PieceType::Knight}) {
        Move m;
        m.from = from;
        m.to = to;
        m.promotion = pt;
        m.kind = kind;
        m.moved_piece = pawn;
        m.captured_piece = captured;
        ml.push(m);
    }
}

// ---------------------------------------------------------------------------
// Piece generators
// ---------------------------------------------------------------------------

void gen_pawn_moves(const Board& b, Square from, Piece pawn, MoveList& ml) noexcept {
    const Color us = pawn.color;
    const Color them = opposite(us);
    const int r = sq_rank(from);
    const int f = sq_file(from);

    // Direction of pawn movement: white goes up (+1 rank), black down (-1).
    const int push_dr = (us == Color::White) ? +1 : -1;
    // Ranks where promotion happens and where double push starts.
    const int promo_rank = (us == Color::White) ? 7 : 0;
    const int start_rank = (us == Color::White) ? 1 : 6;
    const Square ep = b.en_passant_square();

    // ---- Single push ----
    const int tr1 = r + push_dr;
    if (in_bounds(tr1, f)) {
        const Square to1 = make_sq(tr1, f);
        if (b.piece_at(to1).is_none()) {
            if (tr1 == promo_rank) {
                push_promotions(ml, from, to1, pawn, Piece::none(), false);
            } else {
                Move m = quiet(from, to1, pawn);
                // ---- Double push (only from start rank, both squares clear) ----
                if (r == start_rank) {
                    const int tr2 = r + 2 * push_dr;
                    const Square to2 = make_sq(tr2, f);
                    if (b.piece_at(to2).is_none()) {
                        Move dm = m;
                        dm.to = to2;
                        dm.kind = MoveKind::DoublePush;
                        ml.push(dm);
                    }
                }
                ml.push(m);
            }
        }
    }

    // ---- Pawn captures (diagonal) ----
    for (int df : {-1, +1}) {
        const int tf = f + df;
        if (!in_bounds(tr1, tf)) {
            continue;
        }
        const Square to_sq = make_sq(tr1, tf);
        const Piece target = b.piece_at(to_sq);

        if (!target.is_none() && target.color == them) {
            // Normal or promotion-capture.
            if (tr1 == promo_rank) {
                push_promotions(ml, from, to_sq, pawn, target, true);
            } else {
                ml.push(capture(from, to_sq, pawn, target));
            }
        } else if (to_sq == ep) {
            // En-passant: captured pawn is NOT on `to_sq`.
            const Square captured_sq = make_sq(r, tf);
            Move m;
            m.from = from;
            m.to = to_sq;
            m.kind = MoveKind::EnPassant;
            m.moved_piece = pawn;
            m.captured_piece = b.piece_at(captured_sq);
            ml.push(m);
        }
    }
}

void gen_knight_moves(const Board& b, Square from, Piece knight, MoveList& ml) noexcept {
    const int r = sq_rank(from);
    const int f = sq_file(from);
    for (const Delta& d : KNIGHT_JUMPS) {
        const int nr = r + d.dr;
        const int nf = f + d.df;
        if (!in_bounds(nr, nf)) {
            continue;
        }
        const Square to = make_sq(nr, nf);
        const Piece target = b.piece_at(to);
        if (target.is_none()) {
            ml.push(quiet(from, to, knight));
        } else if (target.color != knight.color) {
            ml.push(capture(from, to, knight, target));
        }
    }
}

void gen_sliding_moves(const Board& b, Square from, Piece slider,
                       const Delta dirs[], int n_dirs, MoveList& ml) noexcept {
    const int r0 = sq_rank(from);
    const int f0 = sq_file(from);
    for (int i = 0; i < n_dirs; ++i) {
        int r = r0 + dirs[i].dr;
        int f = f0 + dirs[i].df;
        while (in_bounds(r, f)) {
            const Square to = make_sq(r, f);
            const Piece target = b.piece_at(to);
            if (target.is_none()) {
                ml.push(quiet(from, to, slider));
            } else {
                if (target.color != slider.color) {
                    ml.push(capture(from, to, slider, target));
                }
                break; // Blocked — stop the ray.
            }
            r += dirs[i].dr;
            f += dirs[i].df;
        }
    }
}

// ---------------------------------------------------------------------------
// King moves (non-castling)
// ---------------------------------------------------------------------------

void gen_king_normal_moves(const Board& b, Square from, Piece king, MoveList& ml) noexcept {
    const int r = sq_rank(from);
    const int f = sq_file(from);
    for (const Delta& d : KING_DIRS) {
        const int nr = r + d.dr;
        const int nf = f + d.df;
        if (!in_bounds(nr, nf)) {
            continue;
        }
        const Square to = make_sq(nr, nf);
        const Piece target = b.piece_at(to);
        if (target.is_none()) {
            ml.push(quiet(from, to, king));
        } else if (target.color != king.color) {
            ml.push(capture(from, to, king, target));
        }
    }
}

// ---------------------------------------------------------------------------
// Castling
//
// Checks that:
//   1. The required castling right exists.
//   2. The squares between king and rook are empty.
//   3. The king does not pass through or land on an attacked square.
//      (The king's starting square must also not be attacked — the king
//      cannot castle while in check.)
//
// We do NOT check that the rook is actually present; a well-formed
// position from FEN guarantees this if the rights are set. A position
// with rights but no rook is already malformed.
// ---------------------------------------------------------------------------

void gen_castling(const Board& b, Square from, Piece king, MoveList& ml) noexcept {
    const Color us = king.color;
    const Color them = opposite(us);
    const CastlingRights cr = b.castling_rights();
    const Rank home_rank = (us == Color::White) ? Rank::R1 : Rank::R8;

    // King-side castle (O-O)
    const bool ks_right = (us == Color::White) ? cr.white_king_side : cr.black_king_side;
    if (ks_right) {
        const Square f_sq = make_square(File::F, home_rank);
        const Square g_sq = make_square(File::G, home_rank);
        if (b.piece_at(f_sq).is_none() && b.piece_at(g_sq).is_none()
            && !MoveGenerator::is_square_attacked(b, from, them)
            && !MoveGenerator::is_square_attacked(b, f_sq, them)
            && !MoveGenerator::is_square_attacked(b, g_sq, them)) {
            Move m;
            m.from = from;
            m.to = g_sq;
            m.kind = MoveKind::KingsideCastle;
            m.moved_piece = king;
            ml.push(m);
        }
    }

    // Queen-side castle (O-O-O)
    const bool qs_right = (us == Color::White) ? cr.white_queen_side : cr.black_queen_side;
    if (qs_right) {
        const Square b_sq = make_square(File::B, home_rank);
        const Square c_sq = make_square(File::C, home_rank);
        const Square d_sq = make_square(File::D, home_rank);
        // B-file only needs to be empty, not unattacked.
        if (b.piece_at(b_sq).is_none() && b.piece_at(c_sq).is_none()
            && b.piece_at(d_sq).is_none()
            && !MoveGenerator::is_square_attacked(b, from, them)
            && !MoveGenerator::is_square_attacked(b, d_sq, them)
            && !MoveGenerator::is_square_attacked(b, c_sq, them)) {
            Move m;
            m.from = from;
            m.to = c_sq;
            m.kind = MoveKind::QueensideCastle;
            m.moved_piece = king;
            ml.push(m);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

MoveList MoveGenerator::generate_pseudo_legal(const Board& b) {
    // Fast path: a BoardBitboard has piece sets already laid
    // out as bitboards. Using the bitboard-native generator
    // avoids the O(64) square walk and the virtual piece_at
    // call inside the loop.
    if (auto* bb = dynamic_cast<const BoardBitboard*>(&b)) {
        return BitboardMoveGenerator::generate_pseudo_legal(*bb);
    }

    MoveList ml;
    const Color us = b.side_to_move();

    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Square sq = static_cast<Square>(i);
        const Piece p = b.piece_at(sq);
        if (p.is_none() || p.color != us) {
            continue;
        }
        switch (p.type) {
            case PieceType::Pawn:
                gen_pawn_moves(b, sq, p, ml);
                break;
            case PieceType::Knight:
                gen_knight_moves(b, sq, p, ml);
                break;
            case PieceType::Bishop:
                gen_sliding_moves(b, sq, p, BISHOP_DIRS, 4, ml);
                break;
            case PieceType::Rook:
                gen_sliding_moves(b, sq, p, ROOK_DIRS, 4, ml);
                break;
            case PieceType::Queen:
                gen_sliding_moves(b, sq, p, BISHOP_DIRS, 4, ml);
                gen_sliding_moves(b, sq, p, ROOK_DIRS, 4, ml);
                break;
            case PieceType::King:
                gen_king_normal_moves(b, sq, p, ml);
                gen_castling(b, sq, p, ml);
                break;
            case PieceType::None:
                break;
        }
    }
    return ml;
}

MoveList MoveGenerator::generate_legal(Board& b) {
    if (auto* bb = dynamic_cast<BoardBitboard*>(&b)) {
        return BitboardMoveGenerator::generate_legal(*bb);
    }

    const MoveList pseudo = generate_pseudo_legal(b);
    // The side whose moves we are filtering — will flip after make_move.
    const Color mover = b.side_to_move();

    MoveList legal;
    for (const Move& m : pseudo) {
        b.make_move(m);
        // After make_move, side_to_move() has flipped. We check whether
        // the side that just moved left their own king in check.
        if (!is_in_check(b, mover)) {
            legal.push(m);
        }
        b.unmake_move(m);
    }
    return legal;
}

bool MoveGenerator::is_in_check(const Board& b, Color side) noexcept {
    if (auto* bb = dynamic_cast<const BoardBitboard*>(&b)) {
        return BitboardMoveGenerator::is_in_check(*bb, side);
    }

    // Find the king's square by scanning the board. O(64) — acceptable.
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Square sq = static_cast<Square>(i);
        const Piece p = b.piece_at(sq);
        if (p.type == PieceType::King && p.color == side) {
            return is_square_attacked(b, sq, opposite(side));
        }
    }
    // Position has no king of `side` — cannot happen in a legal game.
    assert(false && "is_in_check: no king found on the board");
    return false;
}

bool MoveGenerator::is_square_attacked(const Board& b, Square sq,
                                       Color attacker) noexcept {
    if (auto* bb = dynamic_cast<const BoardBitboard*>(&b)) {
        return BitboardMoveGenerator::is_square_attacked(*bb, sq, attacker);
    }

    const int r = sq_rank(sq);
    const int f = sq_file(sq);

    // ---- Pawn attacks ----
    // Look from sq in the direction a pawn of `attacker` color would
    // have come FROM to attack sq.
    // White pawns attack upward (rank+1), so they attack sq from rank-1.
    // Black pawns attack downward (rank-1), so they attack sq from rank+1.
    const int pawn_dr = (attacker == Color::White) ? -1 : +1;
    for (int df : {-1, +1}) {
        const int pr = r + pawn_dr;
        const int pf = f + df;
        if (in_bounds(pr, pf)) {
            const Piece p = b.piece_at(make_sq(pr, pf));
            if (p.type == PieceType::Pawn && p.color == attacker) {
                return true;
            }
        }
    }

    // ---- Knight attacks ----
    for (const Delta& d : KNIGHT_JUMPS) {
        const int nr = r + d.dr;
        const int nf = f + d.df;
        if (in_bounds(nr, nf)) {
            const Piece p = b.piece_at(make_sq(nr, nf));
            if (p.type == PieceType::Knight && p.color == attacker) {
                return true;
            }
        }
    }

    // ---- Rook / Queen (orthogonal rays) ----
    for (const Delta& d : ROOK_DIRS) {
        int cr = r + d.dr;
        int cf = f + d.df;
        while (in_bounds(cr, cf)) {
            const Piece p = b.piece_at(make_sq(cr, cf));
            if (!p.is_none()) {
                if (p.color == attacker
                    && (p.type == PieceType::Rook || p.type == PieceType::Queen)) {
                    return true;
                }
                break;
            }
            cr += d.dr;
            cf += d.df;
        }
    }

    // ---- Bishop / Queen (diagonal rays) ----
    for (const Delta& d : BISHOP_DIRS) {
        int cr = r + d.dr;
        int cf = f + d.df;
        while (in_bounds(cr, cf)) {
            const Piece p = b.piece_at(make_sq(cr, cf));
            if (!p.is_none()) {
                if (p.color == attacker
                    && (p.type == PieceType::Bishop || p.type == PieceType::Queen)) {
                    return true;
                }
                break;
            }
            cr += d.dr;
            cf += d.df;
        }
    }

    // ---- King (one step in any direction) ----
    for (const Delta& d : KING_DIRS) {
        const int nr = r + d.dr;
        const int nf = f + d.df;
        if (in_bounds(nr, nf)) {
            const Piece p = b.piece_at(make_sq(nr, nf));
            if (p.type == PieceType::King && p.color == attacker) {
                return true;
            }
        }
    }

    return false;
}

} // namespace chesserazade

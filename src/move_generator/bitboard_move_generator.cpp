/// Bitboard-native move generator.
///
/// Same output contract as `MoveGenerator` (populates the same
/// `MoveList`), but reads directly from the piece bitboards
/// and per-color occupancy caches that `BoardBitboard` exposes.
/// The speedup over the abstract-interface generator comes from
/// two ideas:
///
///   1. **Parallelism on sets.** Every piece of a given type is
///      processed at once — a single AND with `Attacks::X(sq,
///      occ)` computes *all* destinations, and `pop_lsb` peels
///      them off one by one. We never iterate 64 squares.
///   2. **No virtual dispatch inside the inner loop.** All board
///      reads are direct member accesses on a known concrete
///      type.
///
/// Pawn motion is expressed as whole-board shifts: `pawns << 8`
/// advances every white pawn one rank; `& empty` keeps only the
/// unblocked ones. The shifts use LERF conventions:
///
///     +8  one rank up     (white push)
///     -8  one rank down   (black push)
///     +7  up-and-left     (white capture toward a-file)
///     +9  up-and-right    (white capture toward h-file)
///     -7  down-and-right  (black capture toward h-file)
///     -9  down-and-left   (black capture toward a-file)
///
/// File masks (`& ~FILE_A` before a +7 shift, `& ~FILE_H` before
/// a +9 shift) prevent wrap-around when a pawn sits on the edge.

#include "board/board_bitboard.hpp"

#include <chesserazade/bitboard.hpp>
#include <chesserazade/board.hpp>
#include <chesserazade/move_generator.hpp>

#include <cstddef>
#include <cstdint>

namespace chesserazade {

namespace {

// ---------------------------------------------------------------------------
// Move construction
// ---------------------------------------------------------------------------

Move quiet_move(Square from, Square to, Piece p) noexcept {
    Move m;
    m.from = from; m.to = to; m.kind = MoveKind::Quiet; m.moved_piece = p;
    return m;
}
Move capture_move(Square from, Square to, Piece p, Piece captured) noexcept {
    Move m;
    m.from = from; m.to = to; m.kind = MoveKind::Capture;
    m.moved_piece = p; m.captured_piece = captured;
    return m;
}
Move double_push(Square from, Square to, Piece p) noexcept {
    Move m;
    m.from = from; m.to = to; m.kind = MoveKind::DoublePush;
    m.moved_piece = p;
    return m;
}

/// Append all four promotion variants (Q/R/B/N) to `ml`.
void push_promotions(MoveList& ml, Square from, Square to,
                     Piece pawn, Piece captured, bool is_capture) noexcept {
    const MoveKind kind = is_capture ? MoveKind::PromotionCapture
                                     : MoveKind::Promotion;
    for (PieceType pt : {PieceType::Queen, PieceType::Rook,
                         PieceType::Bishop, PieceType::Knight}) {
        Move m;
        m.from = from; m.to = to; m.promotion = pt; m.kind = kind;
        m.moved_piece = pawn; m.captured_piece = captured;
        ml.push(m);
    }
}

// ---------------------------------------------------------------------------
// Piece target-set serialization
// ---------------------------------------------------------------------------
//
// For knights, bishops, rooks, queens, and the king (non-
// castling): given the set of squares they can reach, split it
// into captures and quiet moves and push one `Move` per target.
//
// `their` is the opponent's full occupancy; any target in
// `their` is a capture, any target outside is a quiet move.

void serialize_targets(const BoardBitboard& b,
                       Square from, Piece mover, Bitboard targets,
                       Bitboard their, MoveList& ml) {
    while (targets) {
        const Square to = pop_lsb(targets);
        const Bitboard mask = bb_of(to);
        if (their & mask) {
            ml.push(capture_move(from, to, mover, b.piece_at(to)));
        } else {
            ml.push(quiet_move(from, to, mover));
        }
    }
}

// ---------------------------------------------------------------------------
// Pawns — bulk shift, then serialize
// ---------------------------------------------------------------------------

void gen_pawn_moves(const BoardBitboard& b, Color us, MoveList& ml) {
    const Bitboard pawns = b.pieces(us, PieceType::Pawn);
    if (pawns == 0) return;

    const Bitboard occ   = b.occupancy();
    const Bitboard empty = ~occ;
    const Color them = opposite(us);
    const Bitboard their = b.color_occupancy(them);
    const Square ep = b.en_passant_square();
    const Piece pawn_piece{PieceType::Pawn, us};

    // Colour-dependent directions. We split push / left-capture /
    // right-capture into the two actual bitboard expressions up
    // front so the rest of the function stays branch-free.
    const bool white = us == Color::White;
    const Bitboard promo_rank = white ? RANK_8 : RANK_1;
    const Bitboard mid_rank   = white ? RANK_3 : RANK_6;
    const int push_dir  = white ? +8 : -8;
    const int left_dir  = white ? +7 : -9;   // toward a-file
    const int right_dir = white ? +9 : -7;   // toward h-file

    // --- Single / double pushes (non-capturing) ---------------
    // +7 / +9 / -7 / -9 shifts are gated by file masks to prevent
    // a-file / h-file wrap-around.
    Bitboard single =
        (white ? pawns << 8 : pawns >> 8) & empty;
    // Double push: only the single-push squares that sit on the
    // relay rank (rank 3 for white, rank 6 for black) can push
    // one more rank, and the destination must be empty.
    const Bitboard single_on_relay = single & mid_rank;
    Bitboard dbl =
        (white ? single_on_relay << 8 : single_on_relay >> 8) & empty;

    // Promotions vs ordinary pushes: any single push landing on
    // the promotion rank is a promotion.
    Bitboard single_promo = single & promo_rank;
    single &= ~promo_rank;

    while (single) {
        const Square to = pop_lsb(single);
        const Square from = static_cast<Square>(to_index(to) - push_dir);
        ml.push(quiet_move(from, to, pawn_piece));
    }
    while (single_promo) {
        const Square to = pop_lsb(single_promo);
        const Square from = static_cast<Square>(to_index(to) - push_dir);
        push_promotions(ml, from, to, pawn_piece, Piece::none(), false);
    }
    while (dbl) {
        const Square to = pop_lsb(dbl);
        const Square from =
            static_cast<Square>(to_index(to) - 2 * push_dir);
        ml.push(double_push(from, to, pawn_piece));
    }

    // --- Captures (left / right) ------------------------------
    Bitboard left_caps = white
        ? (pawns & ~FILE_A) << 7 & their
        : (pawns & ~FILE_A) >> 9 & their;
    Bitboard right_caps = white
        ? (pawns & ~FILE_H) << 9 & their
        : (pawns & ~FILE_H) >> 7 & their;

    Bitboard left_promos  = left_caps  & promo_rank;
    Bitboard right_promos = right_caps & promo_rank;
    left_caps  &= ~promo_rank;
    right_caps &= ~promo_rank;

    while (left_caps) {
        const Square to = pop_lsb(left_caps);
        const Square from = static_cast<Square>(to_index(to) - left_dir);
        ml.push(capture_move(from, to, pawn_piece, b.piece_at(to)));
    }
    while (right_caps) {
        const Square to = pop_lsb(right_caps);
        const Square from = static_cast<Square>(to_index(to) - right_dir);
        ml.push(capture_move(from, to, pawn_piece, b.piece_at(to)));
    }
    while (left_promos) {
        const Square to = pop_lsb(left_promos);
        const Square from = static_cast<Square>(to_index(to) - left_dir);
        push_promotions(ml, from, to, pawn_piece, b.piece_at(to), true);
    }
    while (right_promos) {
        const Square to = pop_lsb(right_promos);
        const Square from = static_cast<Square>(to_index(to) - right_dir);
        push_promotions(ml, from, to, pawn_piece, b.piece_at(to), true);
    }

    // --- En passant ------------------------------------------
    if (ep != Square::None) {
        // Our pawns that attack `ep` are exactly the pawns that
        // a pawn of the opposite color, sitting on `ep`, would
        // attack (the attack pattern is symmetric under color-
        // swap through the reverse direction).
        Bitboard ep_attackers = pawns & Attacks::pawn(them, ep);
        while (ep_attackers) {
            const Square from = pop_lsb(ep_attackers);
            Move m;
            m.from = from; m.to = ep; m.kind = MoveKind::EnPassant;
            m.moved_piece = pawn_piece;
            // The captured pawn sits on the capturing pawn's rank,
            // at the file of `ep`.
            const Square captured_sq =
                make_square(file_of(ep), rank_of(from));
            m.captured_piece = b.piece_at(captured_sq);
            ml.push(m);
        }
    }
}

// ---------------------------------------------------------------------------
// Castling
// ---------------------------------------------------------------------------

void gen_castling(const BoardBitboard& b, Color us, Square king_from,
                  MoveList& ml) {
    const Color them = opposite(us);
    const CastlingRights cr = b.castling_rights();
    const Rank home = us == Color::White ? Rank::R1 : Rank::R8;
    const Piece king{PieceType::King, us};
    const Bitboard occ = b.occupancy();

    const bool ks = us == Color::White ? cr.white_king_side
                                       : cr.black_king_side;
    if (ks) {
        const Square f_sq = make_square(File::F, home);
        const Square g_sq = make_square(File::G, home);
        const Bitboard path = bb_of(f_sq) | bb_of(g_sq);
        if ((occ & path) == 0
            && !BitboardMoveGenerator::is_square_attacked(b, king_from, them)
            && !BitboardMoveGenerator::is_square_attacked(b, f_sq,     them)
            && !BitboardMoveGenerator::is_square_attacked(b, g_sq,     them)) {
            Move m;
            m.from = king_from; m.to = g_sq;
            m.kind = MoveKind::KingsideCastle; m.moved_piece = king;
            ml.push(m);
        }
    }

    const bool qs = us == Color::White ? cr.white_queen_side
                                       : cr.black_queen_side;
    if (qs) {
        const Square b_sq = make_square(File::B, home);
        const Square c_sq = make_square(File::C, home);
        const Square d_sq = make_square(File::D, home);
        const Bitboard path = bb_of(b_sq) | bb_of(c_sq) | bb_of(d_sq);
        if ((occ & path) == 0
            && !BitboardMoveGenerator::is_square_attacked(b, king_from, them)
            && !BitboardMoveGenerator::is_square_attacked(b, d_sq,     them)
            && !BitboardMoveGenerator::is_square_attacked(b, c_sq,     them)) {
            Move m;
            m.from = king_from; m.to = c_sq;
            m.kind = MoveKind::QueensideCastle; m.moved_piece = king;
            ml.push(m);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool BitboardMoveGenerator::is_square_attacked(const BoardBitboard& b,
                                               Square sq,
                                               Color attacker) noexcept {
    const Bitboard occ = b.occupancy();
    const Bitboard pawns = b.pieces(attacker, PieceType::Pawn);
    // Pawns attacking `sq`: look at the squares from which a
    // pawn of `attacker` would attack `sq`. Equivalent to
    // `Attacks::pawn(opposite(attacker), sq)` — the reverse
    // pattern.
    if (Attacks::pawn(opposite(attacker), sq) & pawns) return true;
    if (Attacks::knight(sq) & b.pieces(attacker, PieceType::Knight))
        return true;
    if (Attacks::king(sq) & b.pieces(attacker, PieceType::King))
        return true;

    const Bitboard rook_queens =
        b.pieces(attacker, PieceType::Rook)
        | b.pieces(attacker, PieceType::Queen);
    if (Attacks::rook(sq, occ) & rook_queens) return true;

    const Bitboard bishop_queens =
        b.pieces(attacker, PieceType::Bishop)
        | b.pieces(attacker, PieceType::Queen);
    if (Attacks::bishop(sq, occ) & bishop_queens) return true;

    return false;
}

bool BitboardMoveGenerator::is_in_check(const BoardBitboard& b,
                                        Color side) noexcept {
    const Bitboard king = b.pieces(side, PieceType::King);
    if (king == 0) return false; // malformed, but no check.
    return is_square_attacked(b, lsb(king), opposite(side));
}

MoveList BitboardMoveGenerator::generate_pseudo_legal(const BoardBitboard& b) {
    MoveList ml;
    const Color us = b.side_to_move();
    const Bitboard our = b.color_occupancy(us);
    const Color them = opposite(us);
    const Bitboard their = b.color_occupancy(them);
    const Bitboard occ = b.occupancy();

    gen_pawn_moves(b, us, ml);

    // Knights.
    Bitboard bb = b.pieces(us, PieceType::Knight);
    while (bb) {
        const Square from = pop_lsb(bb);
        const Piece p{PieceType::Knight, us};
        const Bitboard targets = Attacks::knight(from) & ~our;
        serialize_targets(b, from, p, targets, their, ml);
    }

    // Bishops.
    bb = b.pieces(us, PieceType::Bishop);
    while (bb) {
        const Square from = pop_lsb(bb);
        const Piece p{PieceType::Bishop, us};
        const Bitboard targets = Attacks::bishop(from, occ) & ~our;
        serialize_targets(b, from, p, targets, their, ml);
    }

    // Rooks.
    bb = b.pieces(us, PieceType::Rook);
    while (bb) {
        const Square from = pop_lsb(bb);
        const Piece p{PieceType::Rook, us};
        const Bitboard targets = Attacks::rook(from, occ) & ~our;
        serialize_targets(b, from, p, targets, their, ml);
    }

    // Queens.
    bb = b.pieces(us, PieceType::Queen);
    while (bb) {
        const Square from = pop_lsb(bb);
        const Piece p{PieceType::Queen, us};
        const Bitboard targets = Attacks::queen(from, occ) & ~our;
        serialize_targets(b, from, p, targets, their, ml);
    }

    // King.
    const Bitboard kings = b.pieces(us, PieceType::King);
    if (kings) {
        const Square from = lsb(kings);
        const Piece p{PieceType::King, us};
        const Bitboard targets = Attacks::king(from) & ~our;
        serialize_targets(b, from, p, targets, their, ml);
        gen_castling(b, us, from, ml);
    }

    return ml;
}

MoveList BitboardMoveGenerator::generate_legal(BoardBitboard& b) {
    const MoveList pseudo = generate_pseudo_legal(b);
    const Color mover = b.side_to_move();

    MoveList legal;
    for (const Move& m : pseudo) {
        b.make_move(m);
        if (!is_in_check(b, mover)) {
            legal.push(m);
        }
        b.unmake_move(m);
    }
    return legal;
}

} // namespace chesserazade

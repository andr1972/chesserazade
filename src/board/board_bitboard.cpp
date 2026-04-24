#include "board/board_bitboard.hpp"

#include "io/fen_fields.hpp"

#include <chesserazade/evaluator.hpp>
#include <chesserazade/zobrist.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace chesserazade {

void BoardBitboard::clear() noexcept {
    for (auto& row : pieces_) row.fill(0);
    color_.fill(0);
    occ_ = 0;
    side_ = Color::White;
    castling_ = CastlingRights{};
    ep_ = Square::None;
    halfmove_ = 0;
    fullmove_ = 1;
    zobrist_ = 0;
    eval_score_ = 0;
    history_.clear();
}

int BoardBitboard::evaluate_incremental() const noexcept {
    return (side_ == Color::White) ? eval_score_ : -eval_score_;
}

void BoardBitboard::recompute_zobrist() noexcept {
    zobrist_ = compute_zobrist_key(*this);
}

void BoardBitboard::recompute_eval() noexcept {
    int s = 0;
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Square sq = static_cast<Square>(i);
        s += piece_contribution(piece_at(sq), sq);
    }
    eval_score_ = s;
}

void BoardBitboard::set_piece_at(Square s, Piece p) noexcept {
    const Bitboard mask = bb_of(s);

    // Clear any previous occupant of `s` from every piece /
    // color bitboard. Set-to-empty on an empty square is a
    // no-op since all AND-NOT-mask clears the bit that wasn't
    // there.
    for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t pt = 1; pt < 7; ++pt) {
            pieces_[c][pt] &= ~mask;
        }
        color_[c] &= ~mask;
    }
    occ_ &= ~mask;

    if (!p.is_none()) {
        const auto c = static_cast<std::size_t>(p.color);
        const auto pt = static_cast<std::size_t>(p.type);
        pieces_[c][pt] |= mask;
        color_[c] |= mask;
        occ_ |= mask;
    }
}

Piece BoardBitboard::piece_at(Square s) const noexcept {
    const Bitboard mask = bb_of(s);
    if ((occ_ & mask) == 0) return Piece::none();

    // Which color occupies the square? At most one.
    const Color c = (color_[0] & mask) != 0 ? Color::White : Color::Black;
    const auto ci = static_cast<std::size_t>(c);

    // Which piece type? Linear walk — exactly 6 iterations
    // worst case, and the branch predictor loves it because the
    // answer is the same piece type for many adjacent queries
    // during move generation.
    for (std::size_t pt = 1; pt < 7; ++pt) {
        if ((pieces_[ci][pt] & mask) != 0) {
            return Piece{static_cast<PieceType>(pt), c};
        }
    }
    // Occupancy said "yes" but no piece type had the square —
    // an invariant violation. Return empty so the caller gets
    // defined behavior; an assert above is also reasonable.
    return Piece::none();
}

// ---------------------------------------------------------------------------
// Private piece-movement helpers
// ---------------------------------------------------------------------------

void BoardBitboard::remove_piece(Square s, Piece p) noexcept {
    const Bitboard mask = bb_of(s);
    const auto c = static_cast<std::size_t>(p.color);
    const auto pt = static_cast<std::size_t>(p.type);
    pieces_[c][pt] &= ~mask;
    color_[c]    &= ~mask;
    occ_         &= ~mask;
    zobrist_     ^= Zobrist::piece(p, s);
    eval_score_  -= piece_contribution(p, s);
}

void BoardBitboard::add_piece(Square s, Piece p) noexcept {
    const Bitboard mask = bb_of(s);
    const auto c = static_cast<std::size_t>(p.color);
    const auto pt = static_cast<std::size_t>(p.type);
    pieces_[c][pt] |= mask;
    color_[c]    |= mask;
    occ_         |= mask;
    zobrist_     ^= Zobrist::piece(p, s);
    eval_score_  += piece_contribution(p, s);
}

void BoardBitboard::move_piece(Square from, Square to, Piece p) noexcept {
    remove_piece(from, p);
    add_piece(to, p);
}

// ---------------------------------------------------------------------------
// make_move / unmake_move
// ---------------------------------------------------------------------------
//
// Mirrors Board8x8Mailbox::make_move step for step; the only
// difference is that piece manipulation goes through the
// bitboard helpers above instead of writing a single array slot.
// The Zobrist key is maintained incrementally — XOR out the
// pre-move EP / castling / side bits, apply all piece moves
// (each of which XORs its own piece contribution), then XOR in
// the post-move EP / castling. The side-to-move bit was XORed
// once at the top and never re-added, which is exactly the
// toggle we want.

void BoardBitboard::make_move(const Move& m) noexcept {
    assert(is_valid(m.from) && is_valid(m.to));

    // 1. Save irrecoverable state (incl. pre-move Zobrist).
    history_.push_back({ep_, castling_, halfmove_, zobrist_});

    // 2. XOR out the pre-move non-piece Zobrist contributions;
    //    the new values are XORed back in at the end.
    zobrist_ ^= Zobrist::en_passant(ep_);
    zobrist_ ^= Zobrist::castling(castling_);
    zobrist_ ^= Zobrist::black_to_move(); // toggles side-to-move bit

    ep_ = Square::None;

    // 3. Halfmove clock.
    const bool is_capture = (m.kind == MoveKind::Capture
                             || m.kind == MoveKind::EnPassant
                             || m.kind == MoveKind::PromotionCapture);
    const bool is_pawn = (m.moved_piece.type == PieceType::Pawn);
    if (is_capture || is_pawn) {
        halfmove_ = 0;
    } else {
        ++halfmove_;
    }

    // 4. Execute the move.
    switch (m.kind) {
        case MoveKind::Quiet:
            move_piece(m.from, m.to, m.moved_piece);
            break;

        case MoveKind::Capture:
            remove_piece(m.to, m.captured_piece);
            move_piece(m.from, m.to, m.moved_piece);
            break;

        case MoveKind::DoublePush:
            move_piece(m.from, m.to, m.moved_piece);
            if (m.moved_piece.color == Color::White) {
                ep_ = make_square(file_of(m.from), Rank::R3);
            } else {
                ep_ = make_square(file_of(m.from), Rank::R6);
            }
            break;

        case MoveKind::KingsideCastle: {
            move_piece(m.from, m.to, m.moved_piece); // king e→g
            const Rank r = rank_of(m.from);
            const Piece rook{PieceType::Rook, m.moved_piece.color};
            move_piece(make_square(File::H, r),
                       make_square(File::F, r), rook);
            break;
        }

        case MoveKind::QueensideCastle: {
            move_piece(m.from, m.to, m.moved_piece); // king e→c
            const Rank r = rank_of(m.from);
            const Piece rook{PieceType::Rook, m.moved_piece.color};
            move_piece(make_square(File::A, r),
                       make_square(File::D, r), rook);
            break;
        }

        case MoveKind::EnPassant: {
            // Captured pawn sits on `from`'s rank at `to`'s file.
            const Square captured_sq =
                make_square(file_of(m.to), rank_of(m.from));
            remove_piece(captured_sq, m.captured_piece);
            move_piece(m.from, m.to, m.moved_piece);
            break;
        }

        case MoveKind::Promotion:
            remove_piece(m.from, m.moved_piece);
            add_piece(m.to, Piece{m.promotion, m.moved_piece.color});
            break;

        case MoveKind::PromotionCapture:
            remove_piece(m.to, m.captured_piece);
            remove_piece(m.from, m.moved_piece);
            add_piece(m.to, Piece{m.promotion, m.moved_piece.color});
            break;
    }

    // 5. Update castling rights.
    if (m.moved_piece.type == PieceType::King) {
        if (m.moved_piece.color == Color::White) {
            castling_.white_king_side = false;
            castling_.white_queen_side = false;
        } else {
            castling_.black_king_side = false;
            castling_.black_queen_side = false;
        }
    }
    if (m.moved_piece.type == PieceType::Rook) {
        if (m.from == Square::H1) castling_.white_king_side = false;
        if (m.from == Square::A1) castling_.white_queen_side = false;
        if (m.from == Square::H8) castling_.black_king_side = false;
        if (m.from == Square::A8) castling_.black_queen_side = false;
    }
    // Rook captured on its home corner → revoke the right.
    if (m.to == Square::H1) castling_.white_king_side = false;
    if (m.to == Square::A1) castling_.white_queen_side = false;
    if (m.to == Square::H8) castling_.black_king_side = false;
    if (m.to == Square::A8) castling_.black_queen_side = false;

    // 6. Flip side; increment fullmove after black's move.
    if (side_ == Color::Black) {
        ++fullmove_;
    }
    side_ = opposite(side_);

    // 7. XOR in the post-move non-piece Zobrist contributions.
    //    black-to-move was toggled once at step 2.
    zobrist_ ^= Zobrist::en_passant(ep_);
    zobrist_ ^= Zobrist::castling(castling_);
}

void BoardBitboard::unmake_move(const Move& m) noexcept {
    assert(!history_.empty() && "unmake_move: no matching make_move");
    assert(is_valid(m.from) && is_valid(m.to));

    // Flip side first; the rest of the restore uses the pre-move side.
    side_ = opposite(side_);
    if (side_ == Color::Black) {
        --fullmove_;
    }

    // Pop snapshot (restores ep / castling / halfmove / zobrist
    // in one shot — much simpler than re-XORing them in).
    const StateSnapshot snap = history_.back();
    history_.pop_back();
    ep_ = snap.ep;
    castling_ = snap.castling;
    halfmove_ = snap.halfmove;

    // Reverse piece motion. `move_piece` / `add_piece` /
    // `remove_piece` all XOR the Zobrist key, so using them
    // here would drift it — we restore the key wholesale from
    // the snapshot *after* this block. Meanwhile we use the
    // helpers for their bitboard-update side effects; their
    // Zobrist XORs cancel out because we restore the saved
    // value at the end.
    switch (m.kind) {
        case MoveKind::Quiet:
        case MoveKind::DoublePush:
            move_piece(m.to, m.from, m.moved_piece);
            break;

        case MoveKind::Capture:
            move_piece(m.to, m.from, m.moved_piece);
            add_piece(m.to, m.captured_piece);
            break;

        case MoveKind::KingsideCastle: {
            move_piece(m.to, m.from, m.moved_piece); // king back
            const Rank r = rank_of(m.from);
            const Piece rook{PieceType::Rook, m.moved_piece.color};
            move_piece(make_square(File::F, r),
                       make_square(File::H, r), rook);
            break;
        }

        case MoveKind::QueensideCastle: {
            move_piece(m.to, m.from, m.moved_piece);
            const Rank r = rank_of(m.from);
            const Piece rook{PieceType::Rook, m.moved_piece.color};
            move_piece(make_square(File::D, r),
                       make_square(File::A, r), rook);
            break;
        }

        case MoveKind::EnPassant: {
            const Square captured_sq =
                make_square(file_of(m.to), rank_of(m.from));
            move_piece(m.to, m.from, m.moved_piece);
            add_piece(captured_sq, m.captured_piece);
            break;
        }

        case MoveKind::Promotion: {
            remove_piece(m.to, Piece{m.promotion, m.moved_piece.color});
            add_piece(m.from, m.moved_piece);
            break;
        }

        case MoveKind::PromotionCapture: {
            remove_piece(m.to, Piece{m.promotion, m.moved_piece.color});
            add_piece(m.from, m.moved_piece);
            add_piece(m.to, m.captured_piece);
            break;
        }
    }

    // Restore the exact pre-move Zobrist key rather than trust
    // the drifting incremental one (see comment above).
    zobrist_ = snap.zobrist;
}

// ---------------------------------------------------------------------------
// Null move — "pass" for null-move pruning.
// ---------------------------------------------------------------------------
//
// No piece moves, so no bitboards, no eval_score_, no material
// counters need touching. Only `ep_` (cleared — losing the EP
// right is a consequence of not replying to the opponent's last
// move) and `side_` flip, with matching Zobrist XORs. Snapshot
// is the same small struct used by make_move.
//
void BoardBitboard::make_null_move() noexcept {
    history_.push_back({ep_, castling_, halfmove_, zobrist_});

    zobrist_ ^= Zobrist::en_passant(ep_);
    zobrist_ ^= Zobrist::black_to_move();

    ep_ = Square::None;
    ++halfmove_;
    if (side_ == Color::Black) ++fullmove_;
    side_ = opposite(side_);

    zobrist_ ^= Zobrist::en_passant(ep_); // no-op; kept for symmetry
}

void BoardBitboard::unmake_null_move() noexcept {
    assert(!history_.empty()
           && "unmake_null_move: no matching make_null_move");

    if (side_ == Color::White) --fullmove_;
    side_ = opposite(side_);

    const StateSnapshot snap = history_.back();
    history_.pop_back();
    ep_ = snap.ep;
    castling_ = snap.castling;
    halfmove_ = snap.halfmove;
    zobrist_ = snap.zobrist;
}

// ---------------------------------------------------------------------------
// FEN factory
// ---------------------------------------------------------------------------

std::expected<BoardBitboard, FenError>
BoardBitboard::from_fen(std::string_view fen) {
    auto fields = parse_fen_fields(fen);
    if (!fields) return std::unexpected(fields.error());

    BoardBitboard b;
    b.clear();
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Piece p = fields->squares[i];
        if (!p.is_none()) {
            b.set_piece_at(static_cast<Square>(i), p);
        }
    }
    b.set_side_to_move(fields->side);
    b.set_castling_rights(fields->castling);
    b.set_en_passant_square(fields->ep);
    b.set_halfmove_clock(fields->halfmove);
    b.set_fullmove_number(fields->fullmove);
    b.recompute_zobrist();
    b.recompute_eval();
    return b;
}

} // namespace chesserazade

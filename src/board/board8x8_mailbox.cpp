#include "board/board8x8_mailbox.hpp"

#include <cassert>

namespace chesserazade {

Piece Board8x8Mailbox::piece_at(Square s) const noexcept {
    assert(is_valid(s) && "Board8x8Mailbox::piece_at: square must be 0..63");
    return squares_[to_index(s)];
}

void Board8x8Mailbox::clear() noexcept {
    squares_.fill(Piece::none());
    side_to_move_ = Color::White;
    castling_ = CastlingRights{};
    ep_square_ = Square::None;
    halfmove_clock_ = 0;
    fullmove_number_ = 1;
    history_.clear();
}

void Board8x8Mailbox::set_piece_at(Square s, Piece p) noexcept {
    assert(is_valid(s) && "Board8x8Mailbox::set_piece_at: square must be 0..63");
    squares_[to_index(s)] = p;
}

// ---------------------------------------------------------------------------
// make_move
// ---------------------------------------------------------------------------
//
// Applies a legal move. The caller (move generator / search) guarantees
// legality; we assert basic invariants but do not re-check check-legality.
//
// Execution order:
//  1. Push a StateSnapshot so unmake_move can restore irrecoverable state.
//  2. Clear the EP target (every move resets it; DoublePush sets it anew).
//  3. Update the halfmove clock (reset on capture or pawn move).
//  4. Execute piece relocation and any special-move side-effects.
//  5. Update castling rights based on what moved or was captured.
//  6. Flip side to move; increment fullmove number on black's move.
//
void Board8x8Mailbox::make_move(const Move& m) noexcept {
    assert(is_valid(m.from) && is_valid(m.to));

    // 1. Save irrecoverable state.
    history_.push_back({ep_square_, castling_, halfmove_clock_});

    // 2. Reset EP target (overwritten below if DoublePush).
    ep_square_ = Square::None;

    // 3. Halfmove clock: reset on pawn move or capture.
    const bool is_capture = (m.kind == MoveKind::Capture
                             || m.kind == MoveKind::EnPassant
                             || m.kind == MoveKind::PromotionCapture);
    const bool is_pawn = (m.moved_piece.type == PieceType::Pawn);
    if (is_capture || is_pawn) {
        halfmove_clock_ = 0;
    } else {
        ++halfmove_clock_;
    }

    // 4. Execute the move.
    squares_[to_index(m.from)] = Piece::none();

    switch (m.kind) {
        case MoveKind::Quiet:
        case MoveKind::Capture:
            squares_[to_index(m.to)] = m.moved_piece;
            break;

        case MoveKind::DoublePush:
            squares_[to_index(m.to)] = m.moved_piece;
            // Set EP target to the square the pawn skipped over.
            // White pawn moves from rank 2 to rank 4, target is rank 3.
            // Black pawn moves from rank 7 to rank 5, target is rank 6.
            if (m.moved_piece.color == Color::White) {
                ep_square_ = make_square(file_of(m.from), Rank::R3);
            } else {
                ep_square_ = make_square(file_of(m.from), Rank::R6);
            }
            break;

        case MoveKind::KingsideCastle: {
            squares_[to_index(m.to)] = m.moved_piece; // king to g1/g8
            // Rook from h1/h8 to f1/f8.
            const Rank r = rank_of(m.from);
            const Square rook_from = make_square(File::H, r);
            const Square rook_to = make_square(File::F, r);
            squares_[to_index(rook_from)] = Piece::none();
            squares_[to_index(rook_to)] = Piece{PieceType::Rook, m.moved_piece.color};
            break;
        }

        case MoveKind::QueensideCastle: {
            squares_[to_index(m.to)] = m.moved_piece; // king to c1/c8
            // Rook from a1/a8 to d1/d8.
            const Rank r = rank_of(m.from);
            const Square rook_from = make_square(File::A, r);
            const Square rook_to = make_square(File::D, r);
            squares_[to_index(rook_from)] = Piece::none();
            squares_[to_index(rook_to)] = Piece{PieceType::Rook, m.moved_piece.color};
            break;
        }

        case MoveKind::EnPassant: {
            squares_[to_index(m.to)] = m.moved_piece;
            // Remove the captured pawn, which sits on the same rank as
            // `from`, at the file of `to` (the EP target square is one
            // rank ahead of the captured pawn).
            const Square captured_sq = make_square(file_of(m.to), rank_of(m.from));
            squares_[to_index(captured_sq)] = Piece::none();
            break;
        }

        case MoveKind::Promotion:
            squares_[to_index(m.to)] = Piece{m.promotion, m.moved_piece.color};
            break;

        case MoveKind::PromotionCapture:
            squares_[to_index(m.to)] = Piece{m.promotion, m.moved_piece.color};
            break;
    }

    // 5. Update castling rights.
    //
    // Lose the right for a side when:
    //   * That side's king moves (both rights for that color are gone).
    //   * A rook moves away from its home corner.
    //   * A rook on a corner square is captured (handled by checking `to`).
    //
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
    // If a rook is captured on its home square, revoke the right.
    if (m.to == Square::H1) castling_.white_king_side = false;
    if (m.to == Square::A1) castling_.white_queen_side = false;
    if (m.to == Square::H8) castling_.black_king_side = false;
    if (m.to == Square::A8) castling_.black_queen_side = false;

    // 6. Flip side; increment fullmove after black's move.
    if (side_to_move_ == Color::Black) {
        ++fullmove_number_;
    }
    side_to_move_ = opposite(side_to_move_);
}

// ---------------------------------------------------------------------------
// unmake_move
// ---------------------------------------------------------------------------
//
// Reverses make_move exactly. The caller must pass the same Move object
// that was given to the preceding make_move call.
//
void Board8x8Mailbox::unmake_move(const Move& m) noexcept {
    assert(!history_.empty() && "unmake_move: no matching make_move in history");
    assert(is_valid(m.from) && is_valid(m.to));

    // Flip side first; the rest of the restore logic uses the pre-move side.
    side_to_move_ = opposite(side_to_move_);
    if (side_to_move_ == Color::Black) {
        --fullmove_number_;
    }

    // Pop irrecoverable state.
    const StateSnapshot snap = history_.back();
    history_.pop_back();
    ep_square_ = snap.ep_square;
    castling_ = snap.castling;
    halfmove_clock_ = snap.halfmove_clock;

    // Restore pieces.
    switch (m.kind) {
        case MoveKind::Quiet:
        case MoveKind::Capture:
        case MoveKind::DoublePush:
            squares_[to_index(m.from)] = m.moved_piece;
            squares_[to_index(m.to)] = m.captured_piece; // Piece::none() for non-captures
            break;

        case MoveKind::KingsideCastle: {
            squares_[to_index(m.from)] = m.moved_piece; // king back to e1/e8
            squares_[to_index(m.to)] = Piece::none();   // clear g1/g8
            const Rank r = rank_of(m.from);
            squares_[to_index(make_square(File::H, r))] =
                Piece{PieceType::Rook, m.moved_piece.color}; // rook back to h1/h8
            squares_[to_index(make_square(File::F, r))] = Piece::none(); // clear f1/f8
            break;
        }

        case MoveKind::QueensideCastle: {
            squares_[to_index(m.from)] = m.moved_piece; // king back to e1/e8
            squares_[to_index(m.to)] = Piece::none();   // clear c1/c8
            const Rank r = rank_of(m.from);
            squares_[to_index(make_square(File::A, r))] =
                Piece{PieceType::Rook, m.moved_piece.color}; // rook back to a1/a8
            squares_[to_index(make_square(File::D, r))] = Piece::none(); // clear d1/d8
            break;
        }

        case MoveKind::EnPassant: {
            squares_[to_index(m.from)] = m.moved_piece;
            squares_[to_index(m.to)] = Piece::none();
            // Restore the captured pawn at its original square.
            const Square captured_sq = make_square(file_of(m.to), rank_of(m.from));
            squares_[to_index(captured_sq)] = m.captured_piece;
            break;
        }

        case MoveKind::Promotion:
        case MoveKind::PromotionCapture:
            squares_[to_index(m.from)] = m.moved_piece; // pawn back
            squares_[to_index(m.to)] = m.captured_piece; // restore or clear
            break;
    }
}

} // namespace chesserazade

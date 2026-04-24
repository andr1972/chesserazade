#include "board/board8x8_mailbox.hpp"

#include <chesserazade/evaluator.hpp>
#include <chesserazade/zobrist.hpp>

#include <array>
#include <cassert>
#include <cstdint>

namespace chesserazade {

Piece Board8x8Mailbox::piece_at(Square s) const noexcept {
    assert(is_valid(s) && "Board8x8Mailbox::piece_at: square must be 0..63");
    return squares_[to_index(s)];
}

namespace {

/// Place `p` on square `sq`, keeping the incremental Zobrist key
/// **and** the incremental eval score in sync. Every piece write
/// in `make_move` / `unmake_move` goes through this helper so
/// neither quantity drifts from the board state.
inline void place(std::array<Piece, NUM_SQUARES>& squares,
                  ZobristKey& zob, int& eval_score,
                  Square sq, Piece p) noexcept {
    const std::size_t idx = to_index(sq);
    // Zobrist: XOR out the old piece, XOR in the new.
    zob ^= Zobrist::piece(squares[idx], sq);
    zob ^= Zobrist::piece(p, sq);
    // Eval: subtract the old contribution, add the new. An empty
    // square on either side contributes 0.
    eval_score -= piece_contribution(squares[idx], sq);
    eval_score += piece_contribution(p, sq);
    squares[idx] = p;
}

} // namespace

void Board8x8Mailbox::clear() noexcept {
    squares_.fill(Piece::none());
    side_to_move_ = Color::White;
    castling_ = CastlingRights{};
    ep_square_ = Square::None;
    halfmove_clock_ = 0;
    fullmove_number_ = 1;
    history_.clear();
    zobrist_ = 0;
    eval_score_ = 0;
}

int Board8x8Mailbox::evaluate_incremental() const noexcept {
    return (side_to_move_ == Color::White) ? eval_score_ : -eval_score_;
}

void Board8x8Mailbox::recompute_zobrist() noexcept {
    zobrist_ = compute_zobrist_key(*this);
}

void Board8x8Mailbox::recompute_eval() noexcept {
    int s = 0;
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        s += piece_contribution(squares_[i], static_cast<Square>(i));
    }
    eval_score_ = s;
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

    // 1. Save irrecoverable state (incl. pre-move Zobrist key so
    //    unmake_move can restore it without re-XORing).
    history_.push_back({ep_square_, castling_, halfmove_clock_,
                        zobrist_, eval_score_});

    // 2. XOR out the pre-move non-piece hash contributions so we
    //    can XOR the post-move values back in at the end.
    zobrist_ ^= Zobrist::en_passant(ep_square_);
    zobrist_ ^= Zobrist::castling(castling_);
    zobrist_ ^= Zobrist::black_to_move(); // flipped at step 6

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

    // 4. Execute the move. Every piece write goes through `place`,
    //    which XOR-updates zobrist_ as a side effect.
    place(squares_, zobrist_, eval_score_, m.from, Piece::none());

    switch (m.kind) {
        case MoveKind::Quiet:
        case MoveKind::Capture:
            place(squares_, zobrist_, eval_score_, m.to, m.moved_piece);
            break;

        case MoveKind::DoublePush:
            place(squares_, zobrist_, eval_score_, m.to, m.moved_piece);
            // EP target = the square the pawn skipped over.
            if (m.moved_piece.color == Color::White) {
                ep_square_ = make_square(file_of(m.from), Rank::R3);
            } else {
                ep_square_ = make_square(file_of(m.from), Rank::R6);
            }
            break;

        case MoveKind::KingsideCastle: {
            place(squares_, zobrist_, eval_score_, m.to, m.moved_piece); // king to g1/g8
            const Rank r = rank_of(m.from);
            const Piece rook{PieceType::Rook, m.moved_piece.color};
            place(squares_, zobrist_, eval_score_, make_square(File::H, r), Piece::none());
            place(squares_, zobrist_, eval_score_, make_square(File::F, r), rook);
            break;
        }

        case MoveKind::QueensideCastle: {
            place(squares_, zobrist_, eval_score_, m.to, m.moved_piece); // king to c1/c8
            const Rank r = rank_of(m.from);
            const Piece rook{PieceType::Rook, m.moved_piece.color};
            place(squares_, zobrist_, eval_score_, make_square(File::A, r), Piece::none());
            place(squares_, zobrist_, eval_score_, make_square(File::D, r), rook);
            break;
        }

        case MoveKind::EnPassant: {
            place(squares_, zobrist_, eval_score_, m.to, m.moved_piece);
            // The captured pawn sits on the rank of `from`, at
            // the file of `to` (the EP target is one rank ahead).
            const Square captured_sq =
                make_square(file_of(m.to), rank_of(m.from));
            place(squares_, zobrist_, eval_score_, captured_sq, Piece::none());
            break;
        }

        case MoveKind::Promotion:
        case MoveKind::PromotionCapture:
            place(squares_, zobrist_, eval_score_, m.to,
                  Piece{m.promotion, m.moved_piece.color});
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
    // Rook captured on its home square → revoke the right.
    if (m.to == Square::H1) castling_.white_king_side = false;
    if (m.to == Square::A1) castling_.white_queen_side = false;
    if (m.to == Square::H8) castling_.black_king_side = false;
    if (m.to == Square::A8) castling_.black_queen_side = false;

    // 6. Flip side; increment fullmove after black's move.
    if (side_to_move_ == Color::Black) {
        ++fullmove_number_;
    }
    side_to_move_ = opposite(side_to_move_);

    // 7. XOR in the post-move non-piece hash contributions. The
    //    black-to-move toggle was already XORed at step 2 — that
    //    single XOR flips the side-to-move bit in one go.
    zobrist_ ^= Zobrist::en_passant(ep_square_);
    zobrist_ ^= Zobrist::castling(castling_);
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

    // Pop irrecoverable state. The saved Zobrist key is restored
    // directly rather than re-derived via XORs — it is cheaper
    // and impossible to get wrong.
    const StateSnapshot snap = history_.back();
    history_.pop_back();
    ep_square_ = snap.ep_square;
    castling_ = snap.castling;
    halfmove_clock_ = snap.halfmove_clock;
    zobrist_ = snap.zobrist;
    eval_score_ = snap.eval_score;

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

// ---------------------------------------------------------------------------
// Null move — "pass" for null-move pruning in search.
// ---------------------------------------------------------------------------
//
// Only two fields actually change: `ep_square_` (cleared — the EP
// right is a one-ply privilege of the side that did NOT pass) and
// `side_to_move_` (flipped). Pieces, castling, clocks, eval_score
// all stay put. We still push a full StateSnapshot so unmake_null
// restores zobrist directly rather than re-XOR'ing — same idiom
// as make_move / unmake_move.
//
void Board8x8Mailbox::make_null_move() noexcept {
    history_.push_back({ep_square_, castling_, halfmove_clock_,
                        zobrist_, eval_score_});

    zobrist_ ^= Zobrist::en_passant(ep_square_);
    zobrist_ ^= Zobrist::black_to_move();

    ep_square_ = Square::None;
    ++halfmove_clock_;
    if (side_to_move_ == Color::Black) ++fullmove_number_;
    side_to_move_ = opposite(side_to_move_);

    zobrist_ ^= Zobrist::en_passant(ep_square_); // no-op (None = 0), kept for symmetry
}

void Board8x8Mailbox::unmake_null_move() noexcept {
    assert(!history_.empty()
           && "unmake_null_move: no matching make_null_move");

    if (side_to_move_ == Color::White) --fullmove_number_;
    side_to_move_ = opposite(side_to_move_);

    const StateSnapshot snap = history_.back();
    history_.pop_back();
    ep_square_ = snap.ep_square;
    castling_ = snap.castling;
    halfmove_clock_ = snap.halfmove_clock;
    zobrist_ = snap.zobrist;
    eval_score_ = snap.eval_score;
}

} // namespace chesserazade

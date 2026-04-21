#include "board/board_bitboard.hpp"

#include "io/fen_fields.hpp"

#include <chesserazade/zobrist.hpp>

#include <cassert>
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
    history_.clear();
}

void BoardBitboard::recompute_zobrist() noexcept {
    zobrist_ = compute_zobrist_key(*this);
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
// Mutation (stubs for 1.1.2)
// ---------------------------------------------------------------------------

void BoardBitboard::make_move(const Move& /*m*/) noexcept {
    // 1.1.3 plugs in the real bitboard make/unmake. Until then
    // callers must not mutate a BoardBitboard; the abstract
    // `Board&` contract is enforced at runtime by this assert
    // so a stray caller fails loudly during development.
    assert(false && "BoardBitboard::make_move: not implemented until 1.1.3");
}

void BoardBitboard::unmake_move(const Move& /*m*/) noexcept {
    assert(false && "BoardBitboard::unmake_move: not implemented until 1.1.3");
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
    return b;
}

} // namespace chesserazade

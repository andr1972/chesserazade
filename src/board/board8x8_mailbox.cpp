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
}

void Board8x8Mailbox::set_piece_at(Square s, Piece p) noexcept {
    assert(is_valid(s) && "Board8x8Mailbox::set_piece_at: square must be 0..63");
    squares_[to_index(s)] = p;
}

} // namespace chesserazade

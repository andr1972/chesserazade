#include <chesserazade/move.hpp>

#include <cassert>

namespace chesserazade {

namespace {

/// UCI promotion suffix: lowercase piece letter.
constexpr char promotion_suffix(PieceType pt) noexcept {
    switch (pt) {
        case PieceType::Knight: return 'n';
        case PieceType::Bishop: return 'b';
        case PieceType::Rook:   return 'r';
        case PieceType::Queen:  return 'q';
        default:                return '\0';
    }
}

} // namespace

std::string to_uci(const Move& m) {
    // The Move type allows a Square::None sentinel so default-constructed
    // moves are recognizable. UCI output is only well-defined for real
    // squares, so we assert rather than invent a string.
    assert(is_valid(m.from) && "to_uci: move.from must be a valid square");
    assert(is_valid(m.to) && "to_uci: move.to must be a valid square");

    std::string out;
    out.reserve(5);
    out += to_algebraic(m.from);
    out += to_algebraic(m.to);
    if (m.promotion != PieceType::None) {
        const char suffix = promotion_suffix(m.promotion);
        assert(suffix != '\0' && "to_uci: promotion must be N/B/R/Q");
        out += suffix;
    }
    return out;
}

} // namespace chesserazade

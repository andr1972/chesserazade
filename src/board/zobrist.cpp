/// Zobrist random-constant initialization and feature accessors.
///
/// The constants are laid out in a single `Tables` struct that
/// is built once at program start from a fixed 64-bit seed. Using
/// a `Meyers-singleton`-style local static keeps initialization
/// thread-safe under C++11+ without pulling in a global ctor.

#include <chesserazade/zobrist.hpp>

#include <array>
#include <cstdint>
#include <random>

namespace chesserazade {

namespace {

/// Fixed seed. Changing it is an API break for anything that
/// persists Zobrist keys (opening books, tablebase caches). We
/// do not persist them today, but 0.8+ opening books will.
constexpr std::uint64_t ZOBRIST_SEED = 0xCE55E5A2ADE12345ULL;

struct Tables {
    /// piece_keys[color][type][square]. `PieceType::None` is
    /// left as zero so `piece({None}, s)` contributes nothing.
    std::array<std::array<std::array<ZobristKey, NUM_SQUARES>, 7>, 2> pieces{};

    /// One key per 4-bit KQkq combination (0..15).
    std::array<ZobristKey, 16> castling{};

    /// One key per en-passant *file*. Index 8 is the "no EP"
    /// slot, left as zero.
    std::array<ZobristKey, 9> en_passant{};

    /// Side-to-move toggle, XORed in when it is black's turn.
    ZobristKey black_to_move = 0;

    Tables() {
        std::mt19937_64 rng(ZOBRIST_SEED);

        // Skip PieceType::None (index 0) — leaving it zero means
        // an empty-square lookup contributes nothing, which is
        // exactly what we want for incremental updates.
        for (int c = 0; c < 2; ++c) {
            for (int t = 1; t < 7; ++t) {
                for (std::size_t s = 0; s < NUM_SQUARES; ++s) {
                    pieces[static_cast<std::size_t>(c)]
                          [static_cast<std::size_t>(t)][s] = rng();
                }
            }
        }
        for (std::size_t i = 0; i < castling.size(); ++i) {
            castling[i] = rng();
        }
        // Leave en_passant[8] as zero (no-EP contributes nothing).
        for (std::size_t i = 0; i < 8; ++i) {
            en_passant[i] = rng();
        }
        black_to_move = rng();
    }
};

const Tables& tables() noexcept {
    static const Tables t;
    return t;
}

[[nodiscard]] std::uint8_t castling_index(CastlingRights cr) noexcept {
    std::uint8_t i = 0;
    if (cr.white_king_side)  i |= 0b0001;
    if (cr.white_queen_side) i |= 0b0010;
    if (cr.black_king_side)  i |= 0b0100;
    if (cr.black_queen_side) i |= 0b1000;
    return i;
}

} // namespace

ZobristKey Zobrist::piece(Piece p, Square sq) noexcept {
    if (p.is_none()) return 0;
    const auto& t = tables();
    return t.pieces[static_cast<std::size_t>(p.color)]
                   [static_cast<std::size_t>(p.type)]
                   [to_index(sq)];
}

ZobristKey Zobrist::castling(CastlingRights cr) noexcept {
    return tables().castling[castling_index(cr)];
}

ZobristKey Zobrist::en_passant(Square ep) noexcept {
    if (ep == Square::None) return 0;
    const auto f = static_cast<std::size_t>(file_of(ep));
    return tables().en_passant[f];
}

ZobristKey Zobrist::black_to_move() noexcept {
    return tables().black_to_move;
}

ZobristKey compute_zobrist_key(const Board& board) noexcept {
    ZobristKey k = 0;
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Square sq = static_cast<Square>(i);
        const Piece p = board.piece_at(sq);
        k ^= Zobrist::piece(p, sq);
    }
    k ^= Zobrist::castling(board.castling_rights());
    k ^= Zobrist::en_passant(board.en_passant_square());
    if (board.side_to_move() == Color::Black) {
        k ^= Zobrist::black_to_move();
    }
    return k;
}

} // namespace chesserazade

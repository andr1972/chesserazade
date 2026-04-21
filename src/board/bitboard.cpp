/// Attack-table initialization for non-sliding pieces.
///
/// We build the tables by walking every square and computing
/// the set of destinations by explicit rank / file coordinates
/// — the same "integer bounds-check" idiom the mailbox move
/// generator uses. Correctness-first; a table built in a
/// microsecond once per program startup is nothing compared to
/// the millions of lookups during search.

#include <chesserazade/bitboard.hpp>

#include <array>
#include <cstddef>

namespace chesserazade {

namespace {

struct Tables {
    std::array<Bitboard, NUM_SQUARES> king{};
    std::array<Bitboard, NUM_SQUARES> knight{};
    /// [color][square] — white = 0, black = 1.
    std::array<std::array<Bitboard, NUM_SQUARES>, 2> pawn{};

    Tables() {
        // King: all 8 deltas of magnitude 1 in either axis,
        // bounds-checked. Compute per square.
        constexpr int king_deltas[8][2] = {
            {+1, 0}, {-1, 0}, {0, +1}, {0, -1},
            {+1, +1}, {+1, -1}, {-1, +1}, {-1, -1},
        };
        constexpr int knight_deltas[8][2] = {
            {+2, +1}, {+2, -1}, {-2, +1}, {-2, -1},
            {+1, +2}, {+1, -2}, {-1, +2}, {-1, -2},
        };

        for (int i = 0; i < 64; ++i) {
            const int r = i / 8;
            const int f = i % 8;

            Bitboard k = 0;
            for (const auto& d : king_deltas) {
                const int nr = r + d[0];
                const int nf = f + d[1];
                if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                    k |= Bitboard{1} << (nr * 8 + nf);
                }
            }
            king[static_cast<std::size_t>(i)] = k;

            Bitboard n = 0;
            for (const auto& d : knight_deltas) {
                const int nr = r + d[0];
                const int nf = f + d[1];
                if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                    n |= Bitboard{1} << (nr * 8 + nf);
                }
            }
            knight[static_cast<std::size_t>(i)] = n;

            // Pawn attacks: a pawn of color C on square s
            // attacks the two diagonally-forward squares in
            // the direction C moves. White moves up (+rank),
            // black moves down.
            Bitboard pw = 0;
            Bitboard pb = 0;
            // White pawn attacks — diagonals to +rank.
            if (r + 1 < 8) {
                if (f - 1 >= 0) pw |= Bitboard{1} << ((r + 1) * 8 + (f - 1));
                if (f + 1 < 8)  pw |= Bitboard{1} << ((r + 1) * 8 + (f + 1));
            }
            // Black pawn attacks — diagonals to -rank.
            if (r - 1 >= 0) {
                if (f - 1 >= 0) pb |= Bitboard{1} << ((r - 1) * 8 + (f - 1));
                if (f + 1 < 8)  pb |= Bitboard{1} << ((r - 1) * 8 + (f + 1));
            }
            pawn[0][static_cast<std::size_t>(i)] = pw;
            pawn[1][static_cast<std::size_t>(i)] = pb;
        }
    }
};

const Tables& tables() noexcept {
    static const Tables t;
    return t;
}

} // namespace

Bitboard Attacks::king(Square sq) noexcept {
    return tables().king[to_index(sq)];
}

Bitboard Attacks::knight(Square sq) noexcept {
    return tables().knight[to_index(sq)];
}

Bitboard Attacks::pawn(Color c, Square sq) noexcept {
    return tables().pawn[static_cast<std::size_t>(c)][to_index(sq)];
}

} // namespace chesserazade

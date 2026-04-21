/// Abstract Board interface.
///
/// `Board` is the single shared view that every higher-level subsystem
/// (move generator, evaluator, search, I/O) uses. It exposes enough of
/// a chess position for 0.1 — piece placement, side to move, castling
/// rights, en-passant target, clocks — but no mutators and no
/// `make_move` / `unmake_move` yet; those arrive in 0.2 when there is a
/// move generator to exercise them.
///
/// The interface is abstract so that 1.1 can introduce `BoardBitboard`
/// alongside `Board8x8Mailbox` without touching the rest of the code.
/// Polymorphism is deliberate: the vtable cost is didactically clearer
/// than a template parameter threaded through every module.
///
/// See https://www.chessprogramming.org/Board_Representation for the
/// classical taxonomy of chess board representations.
#pragma once

#include <chesserazade/types.hpp>

namespace chesserazade {

/// Castling rights for both sides, both flanks. The FEN letters
/// correspond to the fields: `K` = white king-side, `Q` = white
/// queen-side, `k` = black king-side, `q` = black queen-side.
struct CastlingRights {
    bool white_king_side = false;
    bool white_queen_side = false;
    bool black_king_side = false;
    bool black_queen_side = false;

    /// True if any castle is still possible. Used by FEN serialization
    /// to emit the `-` placeholder when no rights remain.
    [[nodiscard]] constexpr bool any() const noexcept {
        return white_king_side || white_queen_side
            || black_king_side || black_queen_side;
    }

    friend constexpr bool operator==(const CastlingRights&,
                                     const CastlingRights&) = default;
};

/// Read-only abstract view of a chess position.
///
/// Mutators (`make_move`, `unmake_move`, piece edits) belong to the
/// concrete implementation and are not part of the interface; the
/// move generator in 0.2 will add a small mutation API on the concrete
/// `Board8x8Mailbox` and document it there.
class Board {
public:
    virtual ~Board() = default;

    /// Returns the piece on `s`, or `Piece::none()` if the square is
    /// empty. Behavior is undefined if `s == Square::None`.
    [[nodiscard]] virtual Piece piece_at(Square s) const noexcept = 0;

    /// The color whose turn it is to move.
    [[nodiscard]] virtual Color side_to_move() const noexcept = 0;

    [[nodiscard]] virtual CastlingRights castling_rights() const noexcept = 0;

    /// The square *behind* a pawn that just made a double push, or
    /// `Square::None` if no en-passant capture is available.
    [[nodiscard]] virtual Square en_passant_square() const noexcept = 0;

    /// Half-moves since the last capture or pawn move. Used by the
    /// fifty-move rule. Range: 0..100+.
    [[nodiscard]] virtual int halfmove_clock() const noexcept = 0;

    /// Full-move counter, starting at 1 and incremented after every
    /// black move (standard FEN convention).
    [[nodiscard]] virtual int fullmove_number() const noexcept = 0;

protected:
    Board() = default;
    Board(const Board&) = default;
    Board& operator=(const Board&) = default;
    Board(Board&&) = default;
    Board& operator=(Board&&) = default;
};

} // namespace chesserazade

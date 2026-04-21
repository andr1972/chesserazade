/// Bitboard implementation of the `Board` interface.
///
/// Twelve piece bitboards (pawn/knight/bishop/rook/queen/king ×
/// white/black) plus per-color occupancy caches and a total
/// occupancy. Every piece set is an O(1) bit test; every
/// same-color set is a bitwise-OR of the six piece boards cached
/// incrementally.
///
/// This class parallels `Board8x8Mailbox` — same public surface,
/// same FEN invariants, same Zobrist key — so the engine's
/// move generator, search, and CLI all continue to work against
/// the abstract `Board&` type. The only observable difference
/// is performance, which is the point of 1.1.
///
/// make_move / unmake_move land in 1.1.3 alongside the bitboard
/// move generator; the 1.1.2 stubs assert.
#pragma once

#include <chesserazade/bitboard.hpp>
#include <chesserazade/board.hpp>
#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/types.hpp>

#include <array>
#include <expected>
#include <string_view>
#include <vector>

namespace chesserazade {

class BoardBitboard final : public Board {
public:
    BoardBitboard() = default;

    /// Construct from FEN. Same strict grammar as the mailbox
    /// implementation; both use the shared `parse_fen_fields`.
    [[nodiscard]] static std::expected<BoardBitboard, FenError>
    from_fen(std::string_view fen);

    // Board interface — read-only queries -----------------------------
    [[nodiscard]] Piece piece_at(Square s) const noexcept override;
    [[nodiscard]] Color side_to_move() const noexcept override { return side_; }
    [[nodiscard]] CastlingRights castling_rights() const noexcept override {
        return castling_;
    }
    [[nodiscard]] Square en_passant_square() const noexcept override { return ep_; }
    [[nodiscard]] int halfmove_clock() const noexcept override { return halfmove_; }
    [[nodiscard]] int fullmove_number() const noexcept override { return fullmove_; }
    [[nodiscard]] ZobristKey zobrist_key() const noexcept override { return zobrist_; }

    // Board interface — mutation (stubbed in 1.1.2; real in 1.1.3) ----
    void make_move(const Move& m) noexcept override;
    void unmake_move(const Move& m) noexcept override;

    void recompute_zobrist() noexcept;

    // FEN-level setters ----------------------------------------------
    void clear() noexcept;
    void set_piece_at(Square s, Piece p) noexcept;
    void set_side_to_move(Color c) noexcept { side_ = c; }
    void set_castling_rights(CastlingRights r) noexcept { castling_ = r; }
    void set_en_passant_square(Square s) noexcept { ep_ = s; }
    void set_halfmove_clock(int v) noexcept { halfmove_ = v; }
    void set_fullmove_number(int v) noexcept { fullmove_ = v; }

    // Bitboard accessors (used by the move generator in 1.1.3) -------

    /// All squares holding a piece of the given color+type.
    [[nodiscard]] Bitboard pieces(Color c, PieceType pt) const noexcept {
        return pieces_[static_cast<std::size_t>(c)]
                      [static_cast<std::size_t>(pt)];
    }
    /// All squares holding any piece of `c`.
    [[nodiscard]] Bitboard color_occupancy(Color c) const noexcept {
        return color_[static_cast<std::size_t>(c)];
    }
    /// Every occupied square.
    [[nodiscard]] Bitboard occupancy() const noexcept { return occ_; }

    /// Structural equality — the position fields, not the
    /// (ephemeral) history stack.
    friend bool operator==(const BoardBitboard& a,
                           const BoardBitboard& b) noexcept {
        return a.pieces_ == b.pieces_
            && a.color_ == b.color_
            && a.occ_ == b.occ_
            && a.side_ == b.side_
            && a.castling_ == b.castling_
            && a.ep_ == b.ep_
            && a.halfmove_ == b.halfmove_
            && a.fullmove_ == b.fullmove_;
    }

private:
    /// `pieces_[color][piece_type]`. Slot 0 (`PieceType::None`)
    /// stays zero; it exists only to make indexing by the enum
    /// value direct.
    std::array<std::array<Bitboard, 7>, 2> pieces_{};

    /// Union of all of a color's piece bitboards. Maintained
    /// alongside `pieces_` on every mutation.
    std::array<Bitboard, 2> color_{};

    /// Union of both colors' occupancies.
    Bitboard occ_ = 0;

    Color side_ = Color::White;
    CastlingRights castling_{};
    Square ep_ = Square::None;
    int halfmove_ = 0;
    int fullmove_ = 1;
    ZobristKey zobrist_ = 0;

    // History stack for unmake_move, populated in 1.1.3.
    struct StateSnapshot {
        Square ep = Square::None;
        CastlingRights castling{};
        int halfmove = 0;
        ZobristKey zobrist = 0;
    };
    std::vector<StateSnapshot> history_;
};

} // namespace chesserazade

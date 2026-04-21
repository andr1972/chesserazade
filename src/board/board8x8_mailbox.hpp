/// Mailbox implementation of the `Board` interface.
///
/// A "mailbox" board stores one `Piece` per square in a flat 64-entry
/// array indexed under the little-endian rank-file mapping defined in
/// `types.hpp`. This is the classical introductory representation and
/// the one textbooks describe first (see
/// https://www.chessprogramming.org/Mailbox ). It is not the fastest
/// — 1.1 adds a bitboard implementation — but it is the most
/// transparent, and correctness is easier to reason about here.
///
/// `make_move` / `unmake_move` are implemented by maintaining a small
/// internal history stack of `StateSnapshot` entries. The snapshot
/// captures the position fields that a Move cannot reconstruct on its
/// own: the previous EP square, castling rights, and halfmove clock.
/// Everything else (pieces on squares, side to move, fullmove number)
/// is directly derivable from the Move fields and the current state.
#pragma once

#include <chesserazade/board.hpp>
#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/types.hpp>

#include <array>
#include <expected>
#include <string_view>
#include <vector>

namespace chesserazade {

class Board8x8Mailbox final : public Board {
public:
    /// Default-constructed board is empty with white to move, no
    /// castling rights, no en-passant target, clocks reset. FEN
    /// parsing overwrites every field.
    Board8x8Mailbox() = default;

    /// Construct from FEN. Returns FenError if the string is malformed.
    /// Input is ASCII only — Unicode piece glyphs are rejected.
    [[nodiscard]] static std::expected<Board8x8Mailbox, FenError>
    from_fen(std::string_view fen);

    // Board interface — read-only queries -----------------------------
    [[nodiscard]] Piece piece_at(Square s) const noexcept override;
    [[nodiscard]] Color side_to_move() const noexcept override { return side_to_move_; }
    [[nodiscard]] CastlingRights castling_rights() const noexcept override {
        return castling_;
    }
    [[nodiscard]] Square en_passant_square() const noexcept override { return ep_square_; }
    [[nodiscard]] int halfmove_clock() const noexcept override { return halfmove_clock_; }
    [[nodiscard]] int fullmove_number() const noexcept override { return fullmove_number_; }
    [[nodiscard]] ZobristKey zobrist_key() const noexcept override { return zobrist_; }

    // Board interface — mutation ---------------------------------------
    void make_move(const Move& m) noexcept override;
    void unmake_move(const Move& m) noexcept override;

    /// Recompute the Zobrist key from scratch. Called by the FEN
    /// parser after piece placement; also useful as a rarely-run
    /// consistency check.
    void recompute_zobrist() noexcept;

    // FEN-level setters (used by the FEN parser and unit tests) -------
    void clear() noexcept;
    void set_piece_at(Square s, Piece p) noexcept;
    void set_side_to_move(Color c) noexcept { side_to_move_ = c; }
    void set_castling_rights(CastlingRights r) noexcept { castling_ = r; }
    void set_en_passant_square(Square s) noexcept { ep_square_ = s; }
    void set_halfmove_clock(int v) noexcept { halfmove_clock_ = v; }
    void set_fullmove_number(int v) noexcept { fullmove_number_ = v; }

    /// Position equality is field-wise. Two boards with identical
    /// pieces, side, castling, EP, and clocks compare equal; the
    /// internal history stack is NOT compared (it is ephemeral).
    friend bool operator==(const Board8x8Mailbox& a,
                           const Board8x8Mailbox& b) noexcept {
        return a.squares_ == b.squares_
            && a.side_to_move_ == b.side_to_move_
            && a.castling_ == b.castling_
            && a.ep_square_ == b.ep_square_
            && a.halfmove_clock_ == b.halfmove_clock_
            && a.fullmove_number_ == b.fullmove_number_;
    }

private:
    /// State that is not derivable from the Move alone during unmake.
    /// One entry is pushed by make_move and popped by unmake_move.
    struct StateSnapshot {
        Square ep_square = Square::None;
        CastlingRights castling{};
        int halfmove_clock = 0;
        ZobristKey zobrist = 0;
    };

    std::array<Piece, NUM_SQUARES> squares_{};
    Color side_to_move_ = Color::White;
    CastlingRights castling_{};
    Square ep_square_ = Square::None;
    int halfmove_clock_ = 0;
    int fullmove_number_ = 1;
    ZobristKey zobrist_ = 0;

    /// History stack for unmake_move. Each make_move pushes one entry;
    /// each unmake_move pops one. The depth is bounded by the search
    /// depth, which for a mailbox engine is modest (≤ 100 plies).
    std::vector<StateSnapshot> history_;
};

} // namespace chesserazade

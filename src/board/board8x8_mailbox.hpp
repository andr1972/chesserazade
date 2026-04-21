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
/// The class is a thin struct with getters and setters. FEN I/O drives
/// it directly; later versions layer move generation and make/unmake
/// on top without changing the data layout.
#pragma once

#include <chesserazade/board.hpp>
#include <chesserazade/fen.hpp>
#include <chesserazade/types.hpp>

#include <array>
#include <expected>
#include <string_view>

namespace chesserazade {

class Board8x8Mailbox final : public Board {
public:
    /// Default-constructed board is empty with white to move, no
    /// castling rights, no en-passant target, clocks reset. FEN
    /// parsing overwrites every field.
    Board8x8Mailbox() = default;

    /// Construct a mailbox board from a FEN string. Returns a
    /// human-readable `FenError` if the input is malformed. ASCII
    /// only — Unicode piece glyphs are rejected.
    [[nodiscard]] static std::expected<Board8x8Mailbox, FenError>
    from_fen(std::string_view fen);

    // Board interface -------------------------------------------------
    [[nodiscard]] Piece piece_at(Square s) const noexcept override;
    [[nodiscard]] Color side_to_move() const noexcept override { return side_to_move_; }
    [[nodiscard]] CastlingRights castling_rights() const noexcept override {
        return castling_;
    }
    [[nodiscard]] Square en_passant_square() const noexcept override { return ep_square_; }
    [[nodiscard]] int halfmove_clock() const noexcept override { return halfmove_clock_; }
    [[nodiscard]] int fullmove_number() const noexcept override { return fullmove_number_; }

    // Mutators --------------------------------------------------------

    /// Clear every square and reset state to the same values as a
    /// default-constructed board.
    void clear() noexcept;

    /// Place `p` on `s`. Passing `Piece::none()` empties the square.
    void set_piece_at(Square s, Piece p) noexcept;

    void set_side_to_move(Color c) noexcept { side_to_move_ = c; }
    void set_castling_rights(CastlingRights r) noexcept { castling_ = r; }
    void set_en_passant_square(Square s) noexcept { ep_square_ = s; }
    void set_halfmove_clock(int v) noexcept { halfmove_clock_ = v; }
    void set_fullmove_number(int v) noexcept { fullmove_number_ = v; }

    /// Equality of concrete mailbox boards is field-wise. Two boards
    /// with the same position but different move histories compare
    /// equal — `Board` has no history of its own in 0.1. Written out
    /// by hand because the polymorphic base suppresses `= default`.
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
    std::array<Piece, NUM_SQUARES> squares_{};
    Color side_to_move_ = Color::White;
    CastlingRights castling_{};
    Square ep_square_ = Square::None;
    int halfmove_clock_ = 0;
    int fullmove_number_ = 1;
};

} // namespace chesserazade

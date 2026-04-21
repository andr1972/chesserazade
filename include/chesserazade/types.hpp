/// Core types shared by every module in chesserazade.
///
/// The types here are deliberately small and transparent — the reader
/// of any `.cpp` file should be able to understand a `Square` or a
/// `Piece` without opening a separate document. Everything is plain
/// value types; no inheritance, no templates, no hidden allocations.
///
/// Coordinate conventions:
///   * `File` 0..7 maps to a..h (A=0, H=7).
///   * `Rank` 0..7 maps to 1..8 (R1=0, R8=7).
///   * `Square` = `Rank * 8 + File`, so A1=0, H1=7, A8=56, H8=63.
///     This is the little-endian rank-file mapping (LERF) recommended
///     by the Chess Programming Wiki:
///     https://www.chessprogramming.org/Squares#Little-Endian_Rank-File_Mapping
///
/// Design note: we intentionally do *not* use a bitboard representation
/// in the 0.x line. Bitboards arrive in 1.1 as an alternative `Board`
/// implementation; these types stay unchanged.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace chesserazade {

// ---------------------------------------------------------------------------
// Color
// ---------------------------------------------------------------------------

/// The side that owns a piece or whose turn it is to move.
enum class Color : std::uint8_t {
    White = 0,
    Black = 1,
};

/// Returns the opposite color. White <-> Black.
[[nodiscard]] constexpr Color opposite(Color c) noexcept {
    return (c == Color::White) ? Color::Black : Color::White;
}

// ---------------------------------------------------------------------------
// PieceType
// ---------------------------------------------------------------------------

/// The kind of a piece, independent of color. `None` represents an empty
/// square in data structures that also need to encode empty slots.
enum class PieceType : std::uint8_t {
    None = 0,
    Pawn = 1,
    Knight = 2,
    Bishop = 3,
    Rook = 4,
    Queen = 5,
    King = 6,
};

// ---------------------------------------------------------------------------
// Piece
// ---------------------------------------------------------------------------

/// A chess piece = kind + color. An empty square is `Piece::none()`,
/// whose `type` is `PieceType::None`; its `color` field is unspecified
/// and must not be read.
struct Piece {
    PieceType type = PieceType::None;
    Color color = Color::White;

    /// Sentinel for "no piece on this square".
    [[nodiscard]] static constexpr Piece none() noexcept { return Piece{}; }

    [[nodiscard]] constexpr bool is_none() const noexcept {
        return type == PieceType::None;
    }

    friend constexpr bool operator==(const Piece&, const Piece&) = default;
};

/// Encode a piece as a single FEN ASCII character.
///
/// Uppercase = white, lowercase = black, per the FEN standard. An empty
/// piece is encoded as '.', which is *not* valid FEN but is convenient
/// for board pretty-printing; the FEN serializer emits digit run-lengths
/// instead and never calls this with a `none()` piece.
[[nodiscard]] constexpr char piece_to_fen_char(Piece p) noexcept {
    constexpr std::array<char, 7> white{'.', 'P', 'N', 'B', 'R', 'Q', 'K'};
    constexpr std::array<char, 7> black{'.', 'p', 'n', 'b', 'r', 'q', 'k'};
    const auto idx = static_cast<std::size_t>(p.type);
    return (p.color == Color::White) ? white[idx] : black[idx];
}

/// Parse a FEN ASCII piece character into a `Piece`. Returns `nullopt`
/// for any other character (including digits, '/', and empty square
/// markers like '.').
[[nodiscard]] constexpr std::optional<Piece> piece_from_fen_char(char c) noexcept {
    switch (c) {
        case 'P': return Piece{PieceType::Pawn, Color::White};
        case 'N': return Piece{PieceType::Knight, Color::White};
        case 'B': return Piece{PieceType::Bishop, Color::White};
        case 'R': return Piece{PieceType::Rook, Color::White};
        case 'Q': return Piece{PieceType::Queen, Color::White};
        case 'K': return Piece{PieceType::King, Color::White};
        case 'p': return Piece{PieceType::Pawn, Color::Black};
        case 'n': return Piece{PieceType::Knight, Color::Black};
        case 'b': return Piece{PieceType::Bishop, Color::Black};
        case 'r': return Piece{PieceType::Rook, Color::Black};
        case 'q': return Piece{PieceType::Queen, Color::Black};
        case 'k': return Piece{PieceType::King, Color::Black};
        default:  return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Square, File, Rank
// ---------------------------------------------------------------------------

/// Square index 0..63 under LERF mapping (A1=0, H1=7, A8=56, H8=63).
/// The enum values 0..63 are the valid board squares. The sentinel
/// `None` (64) means "no square" and is used e.g. by the en-passant
/// field when no en-passant target exists.
enum class Square : std::uint8_t {
    A1 =  0, B1, C1, D1, E1, F1, G1, H1,
    A2 =  8, B2, C2, D2, E2, F2, G2, H2,
    A3 = 16, B3, C3, D3, E3, F3, G3, H3,
    A4 = 24, B4, C4, D4, E4, F4, G4, H4,
    A5 = 32, B5, C5, D5, E5, F5, G5, H5,
    A6 = 40, B6, C6, D6, E6, F6, G6, H6,
    A7 = 48, B7, C7, D7, E7, F7, G7, H7,
    A8 = 56, B8, C8, D8, E8, F8, G8, H8,
    None = 64,
};

constexpr std::uint8_t NUM_SQUARES = 64;

/// File (column) index 0..7 for a..h.
enum class File : std::uint8_t { A = 0, B, C, D, E, F, G, H };

/// Rank (row) index 0..7 for 1..8.
enum class Rank : std::uint8_t { R1 = 0, R2, R3, R4, R5, R6, R7, R8 };

[[nodiscard]] constexpr std::uint8_t to_index(Square s) noexcept {
    return static_cast<std::uint8_t>(s);
}

[[nodiscard]] constexpr bool is_valid(Square s) noexcept {
    return to_index(s) < NUM_SQUARES;
}

[[nodiscard]] constexpr Square make_square(File f, Rank r) noexcept {
    return static_cast<Square>(static_cast<std::uint8_t>(r) * 8u
                               + static_cast<std::uint8_t>(f));
}

[[nodiscard]] constexpr File file_of(Square s) noexcept {
    return static_cast<File>(to_index(s) % 8u);
}

[[nodiscard]] constexpr Rank rank_of(Square s) noexcept {
    return static_cast<Rank>(to_index(s) / 8u);
}

[[nodiscard]] constexpr char file_to_char(File f) noexcept {
    return static_cast<char>('a' + static_cast<std::uint8_t>(f));
}

[[nodiscard]] constexpr char rank_to_char(Rank r) noexcept {
    return static_cast<char>('1' + static_cast<std::uint8_t>(r));
}

/// Convert a square to algebraic notation ("a1", "h8").
[[nodiscard]] inline std::string to_algebraic(Square s) {
    return {file_to_char(file_of(s)), rank_to_char(rank_of(s))};
}

/// Parse algebraic-notation square ("a1".."h8"). Case-sensitive: files
/// must be lowercase, ranks must be '1'..'8'. Returns `nullopt` on any
/// malformed input, including wrong length.
[[nodiscard]] constexpr std::optional<Square> square_from_algebraic(
    std::string_view s) noexcept {
    if (s.size() != 2) {
        return std::nullopt;
    }
    const char fc = s[0];
    const char rc = s[1];
    if (fc < 'a' || fc > 'h' || rc < '1' || rc > '8') {
        return std::nullopt;
    }
    const auto f = static_cast<File>(fc - 'a');
    const auto r = static_cast<Rank>(rc - '1');
    return make_square(f, r);
}

} // namespace chesserazade

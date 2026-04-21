/// FEN parser and serializer.
///
/// Parsing strategy: the FEN string is a small, fixed grammar, so a
/// hand-rolled recursive-descent parser is clearer than pulling in a
/// parser library. We split on ASCII whitespace into exactly six
/// fields, then walk each one. Every failure path produces a
/// `FenError` with a specific message rather than a generic "bad FEN".
///
/// Serializer strategy: walk files A..H rank 8..1, flushing runs of
/// empty squares as digits, matching the FEN grammar exactly. Castling
/// rights are emitted in canonical "KQkq" order.

#include <chesserazade/fen.hpp>

#include "board/board8x8_mailbox.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace chesserazade {

namespace {

// ---------------------------------------------------------------------------
// Parser helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::unexpected<FenError> fen_error(std::string msg) {
    return std::unexpected(FenError{std::move(msg)});
}

/// Split on ASCII spaces. Empty tokens (from repeated spaces) are
/// skipped so "r ...  w - -" with extra whitespace still parses.
[[nodiscard]] std::vector<std::string_view> split_whitespace(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') {
            ++i;
        }
        const std::size_t start = i;
        while (i < s.size() && s[i] != ' ') {
            ++i;
        }
        if (start < i) {
            out.emplace_back(s.substr(start, i - start));
        }
    }
    return out;
}

[[nodiscard]] std::expected<int, FenError> parse_nonneg_int(std::string_view s,
                                                            std::string_view field_name) {
    if (s.empty()) {
        return fen_error(std::string{"FEN "} + std::string{field_name} + ": empty integer");
    }
    int value = 0;
    const char* first = s.data();
    const char* last = first + s.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last || value < 0) {
        return fen_error(std::string{"FEN "} + std::string{field_name}
                         + ": not a non-negative integer: '" + std::string{s} + "'");
    }
    return value;
}

/// Parse a single rank string (between '/' delimiters). Writes pieces
/// into squares `[rank*8, rank*8+8)` of `b`.
[[nodiscard]] std::expected<void, FenError> parse_rank(Board8x8Mailbox& b,
                                                      std::string_view rank_str,
                                                      int rank_from_top) {
    const int rank_index = 7 - rank_from_top; // FEN gives rank 8 first.
    int file = 0;
    for (char c : rank_str) {
        if (file > 8) {
            return fen_error("FEN placement: rank " + std::to_string(rank_index + 1)
                             + " has too many files");
        }
        if (c >= '1' && c <= '8') {
            const int run = c - '0';
            if (file + run > 8) {
                return fen_error("FEN placement: rank " + std::to_string(rank_index + 1)
                                 + " digit run overflows");
            }
            file += run;
            continue;
        }
        const auto piece = piece_from_fen_char(c);
        if (!piece.has_value()) {
            return fen_error(std::string{"FEN placement: unexpected character '"} + c + "'");
        }
        if (file >= 8) {
            return fen_error("FEN placement: rank " + std::to_string(rank_index + 1)
                             + " has too many files");
        }
        const auto sq = make_square(static_cast<File>(file), static_cast<Rank>(rank_index));
        b.set_piece_at(sq, *piece);
        ++file;
    }
    if (file != 8) {
        return fen_error("FEN placement: rank " + std::to_string(rank_index + 1)
                         + " has " + std::to_string(file) + " files, expected 8");
    }
    return {};
}

[[nodiscard]] std::expected<void, FenError> parse_placement(Board8x8Mailbox& b,
                                                            std::string_view placement) {
    // Split on '/' into exactly 8 ranks, rank 8 first.
    std::array<std::string_view, 8> ranks{};
    std::size_t start = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i <= placement.size(); ++i) {
        if (i == placement.size() || placement[i] == '/') {
            if (count >= 8) {
                return fen_error("FEN placement: more than 8 ranks");
            }
            ranks[count++] = placement.substr(start, i - start);
            start = i + 1;
        }
    }
    if (count != 8) {
        return fen_error("FEN placement: " + std::to_string(count)
                         + " ranks found, expected 8");
    }
    for (std::size_t i = 0; i < 8; ++i) {
        auto r = parse_rank(b, ranks[i], static_cast<int>(i));
        if (!r.has_value()) {
            return r;
        }
    }
    return {};
}

[[nodiscard]] std::expected<Color, FenError> parse_side(std::string_view s) {
    if (s == "w") {
        return Color::White;
    }
    if (s == "b") {
        return Color::Black;
    }
    return fen_error("FEN side-to-move: expected 'w' or 'b', got '" + std::string{s} + "'");
}

[[nodiscard]] std::expected<CastlingRights, FenError> parse_castling(std::string_view s) {
    if (s == "-") {
        return CastlingRights{};
    }
    if (s.empty() || s.size() > 4) {
        return fen_error("FEN castling: expected '-' or 1..4 of KQkq, got '"
                         + std::string{s} + "'");
    }
    CastlingRights rights{};
    for (char c : s) {
        switch (c) {
            case 'K':
                if (rights.white_king_side) {
                    return fen_error("FEN castling: duplicate 'K'");
                }
                rights.white_king_side = true;
                break;
            case 'Q':
                if (rights.white_queen_side) {
                    return fen_error("FEN castling: duplicate 'Q'");
                }
                rights.white_queen_side = true;
                break;
            case 'k':
                if (rights.black_king_side) {
                    return fen_error("FEN castling: duplicate 'k'");
                }
                rights.black_king_side = true;
                break;
            case 'q':
                if (rights.black_queen_side) {
                    return fen_error("FEN castling: duplicate 'q'");
                }
                rights.black_queen_side = true;
                break;
            default:
                return fen_error(std::string{"FEN castling: invalid character '"} + c + "'");
        }
    }
    return rights;
}

[[nodiscard]] std::expected<Square, FenError> parse_ep(std::string_view s) {
    if (s == "-") {
        return Square::None;
    }
    const auto sq = square_from_algebraic(s);
    if (!sq.has_value()) {
        return fen_error("FEN en-passant: expected '-' or an algebraic square, got '"
                         + std::string{s} + "'");
    }
    const auto r = rank_of(*sq);
    if (r != Rank::R3 && r != Rank::R6) {
        return fen_error("FEN en-passant: target square must be on rank 3 or 6, got '"
                         + std::string{s} + "'");
    }
    return *sq;
}

// ---------------------------------------------------------------------------
// Serializer helpers
// ---------------------------------------------------------------------------

void serialize_placement(const Board& b, std::string& out) {
    for (int rank_index = 7; rank_index >= 0; --rank_index) {
        int empty_run = 0;
        for (int file_index = 0; file_index < 8; ++file_index) {
            const auto sq = make_square(static_cast<File>(file_index),
                                        static_cast<Rank>(rank_index));
            const Piece p = b.piece_at(sq);
            if (p.is_none()) {
                ++empty_run;
                continue;
            }
            if (empty_run > 0) {
                out += static_cast<char>('0' + empty_run);
                empty_run = 0;
            }
            out += piece_to_fen_char(p);
        }
        if (empty_run > 0) {
            out += static_cast<char>('0' + empty_run);
        }
        if (rank_index > 0) {
            out += '/';
        }
    }
}

void serialize_castling(const CastlingRights& r, std::string& out) {
    if (!r.any()) {
        out += '-';
        return;
    }
    if (r.white_king_side) {
        out += 'K';
    }
    if (r.white_queen_side) {
        out += 'Q';
    }
    if (r.black_king_side) {
        out += 'k';
    }
    if (r.black_queen_side) {
        out += 'q';
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::expected<Board8x8Mailbox, FenError> Board8x8Mailbox::from_fen(std::string_view fen) {
    const auto fields = split_whitespace(fen);
    if (fields.size() != 6) {
        return fen_error("FEN: expected 6 whitespace-separated fields, got "
                         + std::to_string(fields.size()));
    }

    Board8x8Mailbox board;
    board.clear();

    if (auto r = parse_placement(board, fields[0]); !r.has_value()) {
        return std::unexpected(r.error());
    }

    if (auto r = parse_side(fields[1])) {
        board.set_side_to_move(*r);
    } else {
        return std::unexpected(r.error());
    }

    if (auto r = parse_castling(fields[2])) {
        board.set_castling_rights(*r);
    } else {
        return std::unexpected(r.error());
    }

    if (auto r = parse_ep(fields[3])) {
        board.set_en_passant_square(*r);
    } else {
        return std::unexpected(r.error());
    }

    if (auto r = parse_nonneg_int(fields[4], "halfmove-clock")) {
        board.set_halfmove_clock(*r);
    } else {
        return std::unexpected(r.error());
    }

    if (auto r = parse_nonneg_int(fields[5], "fullmove-number")) {
        if (*r < 1) {
            return fen_error("FEN fullmove-number: must be at least 1, got "
                             + std::to_string(*r));
        }
        board.set_fullmove_number(*r);
    } else {
        return std::unexpected(r.error());
    }

    // All fields placed via setters; the incremental Zobrist key
    // has not been tracked. Recompute once here.
    board.recompute_zobrist();
    return board;
}

std::string serialize_fen(const Board& b) {
    std::string out;
    out.reserve(96);

    serialize_placement(b, out);
    out += ' ';
    out += (b.side_to_move() == Color::White) ? 'w' : 'b';
    out += ' ';
    serialize_castling(b.castling_rights(), out);
    out += ' ';
    const Square ep = b.en_passant_square();
    if (ep == Square::None) {
        out += '-';
    } else {
        out += to_algebraic(ep);
    }
    out += ' ';
    out += std::to_string(b.halfmove_clock());
    out += ' ';
    out += std::to_string(b.fullmove_number());
    return out;
}

} // namespace chesserazade

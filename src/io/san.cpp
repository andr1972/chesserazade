#include <chesserazade/san.hpp>

#include <chesserazade/board.hpp>
#include <chesserazade/move_generator.hpp>
#include <chesserazade/types.hpp>

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace chesserazade {

namespace {

// ---------------------------------------------------------------------------
// PARSE
// ---------------------------------------------------------------------------
//
// Strategy:
//   1. Strip trailing annotation characters (+, #, !, ?) — we do
//      not demand them and we do not use them beyond rejecting
//      malformed input.
//   2. Detect castling (O-O / O-O-O; 0-0 / 0-0-0 also accepted).
//   3. Peel off the optional "=X" promotion suffix.
//   4. Peel off the target square (last two characters).
//   5. The remaining head is: [piece_letter] [disambig] ["x"].
//      - If the first character is an uppercase piece letter, it is
//        the piece type; otherwise it's a pawn.
//      - The 'x' (capture marker) may appear just before the
//        target; strip it.
//      - What's left is 0, 1, or 2 disambiguator characters:
//        "", "a"..."h", "1"..."8", or "a1"..."h8".
//   6. Enumerate the legal moves in `board` and keep the ones that
//      match the parsed description. Exactly one must remain.
// ---------------------------------------------------------------------------

struct SanParsed {
    PieceType piece = PieceType::Pawn;
    std::optional<File> from_file;
    std::optional<Rank> from_rank;
    bool is_capture = false;
    Square target = Square::None;
    PieceType promotion = PieceType::None;

    enum class CastleKind { None, Kingside, Queenside };
    CastleKind castle = CastleKind::None;
};

[[nodiscard]] std::string_view rstrip_annotations(std::string_view s) noexcept {
    while (!s.empty()) {
        const char c = s.back();
        if (c == '+' || c == '#' || c == '!' || c == '?') {
            s.remove_suffix(1);
            continue;
        }
        break;
    }
    return s;
}

[[nodiscard]] bool is_castle_kingside(std::string_view s) noexcept {
    return s == "O-O" || s == "0-0";
}

[[nodiscard]] bool is_castle_queenside(std::string_view s) noexcept {
    return s == "O-O-O" || s == "0-0-0";
}

[[nodiscard]] std::optional<PieceType> piece_from_san_letter(char c) noexcept {
    switch (c) {
        case 'N': return PieceType::Knight;
        case 'B': return PieceType::Bishop;
        case 'R': return PieceType::Rook;
        case 'Q': return PieceType::Queen;
        case 'K': return PieceType::King;
        default:  return std::nullopt;
    }
}

/// Parse a SAN string into an abstract description; does NOT
/// consult the board to resolve a concrete `Move`. Returns an
/// error only on structural malformation (wrong characters, wrong
/// length).
[[nodiscard]] std::expected<SanParsed, SanError> parse_structure(
    std::string_view text) {
    SanParsed out;

    const std::string_view s = rstrip_annotations(text);
    if (s.empty()) {
        return std::unexpected(SanError{"empty SAN string"});
    }

    // ---- castling ----
    if (is_castle_queenside(s)) {
        out.castle = SanParsed::CastleKind::Queenside;
        return out;
    }
    if (is_castle_kingside(s)) {
        out.castle = SanParsed::CastleKind::Kingside;
        return out;
    }

    // ---- peel off promotion suffix "=X" ----
    std::string_view body = s;
    if (body.size() >= 2 && body[body.size() - 2] == '=') {
        const auto pt = piece_from_san_letter(body.back());
        if (!pt || *pt == PieceType::King || *pt == PieceType::Pawn) {
            return std::unexpected(
                SanError{"invalid promotion piece in '" + std::string{text} + "'"});
        }
        out.promotion = *pt;
        body.remove_suffix(2);
    }

    // ---- target square is the last two characters ----
    if (body.size() < 2) {
        return std::unexpected(
            SanError{"SAN move too short: '" + std::string{text} + "'"});
    }
    const char tf = body[body.size() - 2];
    const char tr = body[body.size() - 1];
    if (tf < 'a' || tf > 'h' || tr < '1' || tr > '8') {
        return std::unexpected(
            SanError{"invalid target square in '" + std::string{text} + "'"});
    }
    out.target = make_square(static_cast<File>(tf - 'a'),
                             static_cast<Rank>(tr - '1'));
    body.remove_suffix(2);

    // ---- piece letter (optional; pawn if absent) ----
    if (!body.empty()) {
        const auto pt = piece_from_san_letter(body.front());
        if (pt) {
            out.piece = *pt;
            body.remove_prefix(1);
        }
    }

    // ---- capture marker 'x' just before the target ----
    if (!body.empty() && body.back() == 'x') {
        out.is_capture = true;
        body.remove_suffix(1);
    }

    // ---- disambiguator: 0, 1, or 2 chars ----
    if (body.size() > 2) {
        return std::unexpected(
            SanError{"unexpected characters in '" + std::string{text} + "'"});
    }
    for (char c : body) {
        if (c >= 'a' && c <= 'h') {
            if (out.from_file.has_value()) {
                return std::unexpected(SanError{
                    "duplicate file disambiguator in '" + std::string{text} + "'"});
            }
            out.from_file = static_cast<File>(c - 'a');
        } else if (c >= '1' && c <= '8') {
            if (out.from_rank.has_value()) {
                return std::unexpected(SanError{
                    "duplicate rank disambiguator in '" + std::string{text} + "'"});
            }
            out.from_rank = static_cast<Rank>(c - '1');
        } else {
            return std::unexpected(SanError{
                "unexpected character in disambiguator of '"
                + std::string{text} + "'"});
        }
    }

    return out;
}

/// Does `m` move the given piece type?  For promotions, `m.moved_piece`
/// is still the pawn (the promotion target is recorded separately).
[[nodiscard]] bool move_piece_matches(const Move& m, PieceType pt) noexcept {
    return m.moved_piece.type == pt;
}

/// Resolve a parsed SAN description against the legal-move list
/// of `board`. Returns the single matching move, or an error.
[[nodiscard]] std::expected<Move, SanError> resolve(
    Board& board, const SanParsed& p, std::string_view original_text) {
    const MoveList legal = MoveGenerator::generate_legal(board);
    const Color us = board.side_to_move();

    if (p.castle != SanParsed::CastleKind::None) {
        const MoveKind want = (p.castle == SanParsed::CastleKind::Kingside)
                                  ? MoveKind::KingsideCastle
                                  : MoveKind::QueensideCastle;
        for (const Move& m : legal) {
            if (m.kind == want && m.moved_piece.color == us) {
                return m;
            }
        }
        return std::unexpected(SanError{"castling not legal here: '"
                                        + std::string{original_text} + "'"});
    }

    const Move* match = nullptr;
    std::size_t n_matches = 0;
    for (const Move& m : legal) {
        if (m.to != p.target) continue;
        if (!move_piece_matches(m, p.piece)) continue;
        if (p.promotion != PieceType::None && m.promotion != p.promotion) continue;
        if (p.promotion == PieceType::None
            && (m.kind == MoveKind::Promotion
                || m.kind == MoveKind::PromotionCapture)) {
            // SAN for a promotion must include "=X".
            continue;
        }
        if (p.from_file && file_of(m.from) != *p.from_file) continue;
        if (p.from_rank && rank_of(m.from) != *p.from_rank) continue;
        ++n_matches;
        match = &m;
    }

    if (n_matches == 0) {
        return std::unexpected(
            SanError{"no legal move matches '" + std::string{original_text} + "'"});
    }
    if (n_matches > 1) {
        return std::unexpected(SanError{"ambiguous SAN '"
                                        + std::string{original_text}
                                        + "' matches multiple legal moves"});
    }
    return *match;
}

// ---------------------------------------------------------------------------
// WRITE
// ---------------------------------------------------------------------------

/// Append `s` to `out`.
void append(std::string& out, std::string_view s) { out.append(s); }

/// Append a 2-char square "a1".."h8".
void append_square(std::string& out, Square sq) {
    out.push_back(static_cast<char>('a' + static_cast<int>(file_of(sq))));
    out.push_back(static_cast<char>('1' + static_cast<int>(rank_of(sq))));
}

[[nodiscard]] char piece_san_letter(PieceType pt) noexcept {
    switch (pt) {
        case PieceType::Knight: return 'N';
        case PieceType::Bishop: return 'B';
        case PieceType::Rook:   return 'R';
        case PieceType::Queen:  return 'Q';
        case PieceType::King:   return 'K';
        default:                return '?';
    }
}

/// Determine the minimal disambiguator needed for `m` among the
/// other legal moves of the same piece type that land on `m.to`.
/// Writes into `out` either nothing, the from-file, the from-rank,
/// or both (the full from-square).
void append_disambiguator(std::string& out, Board& board, const Move& m) {
    const MoveList legal = MoveGenerator::generate_legal(board);

    bool others = false;
    bool file_conflict = false;
    bool rank_conflict = false;
    for (const Move& other : legal) {
        if (other == m) continue;
        if (other.to != m.to) continue;
        if (other.moved_piece.type != m.moved_piece.type) continue;
        if (other.moved_piece.color != m.moved_piece.color) continue;
        others = true;
        if (file_of(other.from) == file_of(m.from)) file_conflict = true;
        if (rank_of(other.from) == rank_of(m.from)) rank_conflict = true;
    }

    if (!others) {
        return;
    }
    if (!file_conflict) {
        out.push_back(static_cast<char>('a' + static_cast<int>(file_of(m.from))));
        return;
    }
    if (!rank_conflict) {
        out.push_back(static_cast<char>('1' + static_cast<int>(rank_of(m.from))));
        return;
    }
    append_square(out, m.from);
}

/// Compute the check/mate suffix by actually playing the move and
/// inspecting the opponent's situation. `board` is mutated and then
/// restored.
[[nodiscard]] const char* check_or_mate_suffix(Board& board, const Move& m) {
    board.make_move(m);
    const Color opponent = board.side_to_move();
    const bool in_check = MoveGenerator::is_in_check(board, opponent);
    const char* suffix = "";
    if (in_check) {
        const MoveList replies = MoveGenerator::generate_legal(board);
        suffix = replies.empty() ? "#" : "+";
    }
    board.unmake_move(m);
    return suffix;
}

} // namespace

// ---------------------------------------------------------------------------
// parse_san
// ---------------------------------------------------------------------------

std::expected<Move, SanError> parse_san(Board& board, std::string_view text) {
    auto parsed = parse_structure(text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return resolve(board, *parsed, text);
}

// ---------------------------------------------------------------------------
// to_san
// ---------------------------------------------------------------------------

std::string to_san(Board& board_before, const Move& m) {
    std::string out;

    if (m.kind == MoveKind::KingsideCastle) {
        out = "O-O";
        append(out, check_or_mate_suffix(board_before, m));
        return out;
    }
    if (m.kind == MoveKind::QueensideCastle) {
        out = "O-O-O";
        append(out, check_or_mate_suffix(board_before, m));
        return out;
    }

    const bool is_capture = (m.kind == MoveKind::Capture
                             || m.kind == MoveKind::EnPassant
                             || m.kind == MoveKind::PromotionCapture);
    const bool is_pawn = (m.moved_piece.type == PieceType::Pawn);

    if (is_pawn) {
        // Pawn moves: no piece letter. For captures we always
        // include the from-file, e.g. "exd5" — this is both
        // standard and unambiguous because the capturing pawn's
        // file identifies it.
        if (is_capture) {
            out.push_back(
                static_cast<char>('a' + static_cast<int>(file_of(m.from))));
            out.push_back('x');
        }
        append_square(out, m.to);
        if (m.promotion != PieceType::None) {
            out.push_back('=');
            out.push_back(piece_san_letter(m.promotion));
        }
    } else {
        out.push_back(piece_san_letter(m.moved_piece.type));
        append_disambiguator(out, board_before, m);
        if (is_capture) {
            out.push_back('x');
        }
        append_square(out, m.to);
    }

    append(out, check_or_mate_suffix(board_before, m));
    return out;
}

} // namespace chesserazade

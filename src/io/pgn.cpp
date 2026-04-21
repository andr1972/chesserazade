#include <chesserazade/pgn.hpp>

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/game.hpp>
#include <chesserazade/san.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chesserazade {

// ---------------------------------------------------------------------------
// Tag accessor
// ---------------------------------------------------------------------------

std::optional<std::string> PgnGame::tag(std::string_view key) const {
    std::optional<std::string> result;
    for (const auto& kv : tags) {
        if (kv.first == key) {
            result = kv.second; // keep going — last occurrence wins
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// PARSER
// ---------------------------------------------------------------------------
//
// A PGN document is a stream of tokens, not a line-based format.
// Comments can span lines; move text and tags can wrap freely.
// We use a small cursor type and a handful of named skippers so the
// main driver loop stays linear and readable.
// ---------------------------------------------------------------------------

namespace {

struct Cursor {
    std::string_view text;
    std::size_t i = 0;

    [[nodiscard]] bool eof() const noexcept { return i >= text.size(); }
    [[nodiscard]] char peek() const noexcept { return text[i]; }
    char consume() noexcept { return text[i++]; }
};

[[nodiscard]] bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void skip_whitespace(Cursor& c) noexcept {
    while (!c.eof() && is_space(c.peek())) {
        c.consume();
    }
}

/// Skip `{…}` brace comment. Consumes the opening `{` and the
/// closing `}`. An unterminated comment returns an error via the
/// out-parameter.
[[nodiscard]] std::expected<void, PgnError> skip_brace_comment(Cursor& c) {
    c.consume(); // '{'
    while (!c.eof()) {
        const char ch = c.consume();
        if (ch == '}') return {};
    }
    return std::unexpected(PgnError{"unterminated brace comment"});
}

/// Skip `;…\n` line comment. Consumes through the newline.
void skip_line_comment(Cursor& c) noexcept {
    c.consume(); // ';'
    while (!c.eof() && c.consume() != '\n') {
        // read until newline
    }
}

/// Skip a variation `(…)` including any nested variations and
/// comments inside. Returns an error if unbalanced.
[[nodiscard]] std::expected<void, PgnError> skip_variation(Cursor& c) {
    int depth = 1;
    c.consume(); // '('
    while (!c.eof() && depth > 0) {
        const char ch = c.peek();
        if (ch == '(') { c.consume(); ++depth; continue; }
        if (ch == ')') { c.consume(); --depth; continue; }
        if (ch == '{') {
            auto r = skip_brace_comment(c);
            if (!r) return r;
            continue;
        }
        if (ch == ';') { skip_line_comment(c); continue; }
        c.consume();
    }
    if (depth != 0) {
        return std::unexpected(PgnError{"unterminated variation"});
    }
    return {};
}

/// Parse a single `[Key "Value"]` tag pair. Assumes the cursor is
/// positioned on `[`. Advances past `]`.
[[nodiscard]] std::expected<std::pair<std::string, std::string>, PgnError>
parse_tag(Cursor& c) {
    c.consume(); // '['
    skip_whitespace(c);

    std::string key;
    while (!c.eof() && !is_space(c.peek()) && c.peek() != '"') {
        key.push_back(c.consume());
    }
    if (key.empty()) {
        return std::unexpected(PgnError{"tag with empty key"});
    }

    skip_whitespace(c);
    if (c.eof() || c.peek() != '"') {
        return std::unexpected(PgnError{"tag '" + key + "' missing quoted value"});
    }
    c.consume(); // opening '"'

    std::string value;
    while (!c.eof() && c.peek() != '"') {
        char ch = c.consume();
        if (ch == '\\' && !c.eof()) {
            // Standard PGN escapes: \" and \\.
            ch = c.consume();
        }
        value.push_back(ch);
    }
    if (c.eof()) {
        return std::unexpected(PgnError{"tag '" + key + "' unterminated value"});
    }
    c.consume(); // closing '"'
    skip_whitespace(c);

    if (c.eof() || c.peek() != ']') {
        return std::unexpected(PgnError{"tag '" + key + "' missing ']'"});
    }
    c.consume(); // ']'
    return std::make_pair(std::move(key), std::move(value));
}

/// Detect the PGN game-termination token. Returns the termination
/// string and advances the cursor past it, or nullopt if the next
/// token is not a termination.
[[nodiscard]] std::optional<std::string> try_consume_termination(Cursor& c) {
    // Check the three multi-char tokens first so "1-0" doesn't
    // get misread when "1/2-1/2" is actually there.
    constexpr std::string_view options[] = {"1/2-1/2", "1-0", "0-1", "*"};
    const std::string_view rest = c.text.substr(c.i);
    for (const auto opt : options) {
        if (rest.starts_with(opt)) {
            c.i += opt.size();
            return std::string{opt};
        }
    }
    return std::nullopt;
}

/// Is `c` a character that may appear inside a SAN token?
[[nodiscard]] bool is_san_char(char c) noexcept {
    // Pieces / files / ranks / '=' (promotion) / 'x' (capture) /
    // '+' and '#' (suffixes) / 'O' '-' (castle) / '0' (digit
    // castle). We explicitly do NOT include '.', which is the
    // turn-dot separator, or whitespace / punctuation.
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    switch (c) {
        case '-': case '=': case '+': case '#':
        case '!': case '?':
            return true;
        default:
            return false;
    }
}

/// Read one SAN-ish token. The cursor must already be positioned
/// on a san character. Stops at the first non-SAN character.
[[nodiscard]] std::string read_san_token(Cursor& c) {
    std::string s;
    while (!c.eof() && is_san_char(c.peek())) {
        s.push_back(c.consume());
    }
    return s;
}

/// Is `s` purely a move-number indicator like "1.", "23.", "1...",
/// "23..."? These are skipped silently.
[[nodiscard]] bool is_move_number(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size() && (s[i] >= '0' && s[i] <= '9')) ++i;
    if (i == 0 || i == s.size()) return false;
    // After the digits we expect only dots.
    for (; i < s.size(); ++i) {
        if (s[i] != '.') return false;
    }
    return true;
}

/// Is `s` a NAG token (`$1`, `$12`, …)?
[[nodiscard]] bool is_nag(std::string_view s) noexcept {
    if (s.size() < 2 || s.front() != '$') return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

} // namespace

std::expected<PgnGame, PgnError> parse_pgn(std::string_view text) {
    PgnGame out;
    Cursor c{text, 0};

    // ---- 1. Tag pairs ---------------------------------------------------
    while (true) {
        skip_whitespace(c);
        if (c.eof()) break;
        if (c.peek() != '[') break;
        auto tag = parse_tag(c);
        if (!tag) return std::unexpected(tag.error());
        if (tag->first == "FEN") {
            out.starting_fen = tag->second;
        }
        out.tags.push_back(std::move(*tag));
    }

    // ---- 2. Set up the replay board -------------------------------------
    const std::string starting_fen =
        out.starting_fen.value_or(std::string{STARTING_POSITION_FEN});
    auto board_r = Board8x8Mailbox::from_fen(starting_fen);
    if (!board_r) {
        return std::unexpected(
            PgnError{"starting FEN invalid: " + board_r.error().message});
    }
    Board8x8Mailbox replay = *board_r;

    // ---- 3. Move text ---------------------------------------------------
    bool found_termination = false;
    while (!c.eof() && !found_termination) {
        skip_whitespace(c);
        if (c.eof()) break;

        const char ch = c.peek();

        if (ch == '{') {
            auto r = skip_brace_comment(c);
            if (!r) return std::unexpected(r.error());
            continue;
        }
        if (ch == ';') {
            skip_line_comment(c);
            continue;
        }
        if (ch == '(') {
            auto r = skip_variation(c);
            if (!r) return std::unexpected(r.error());
            continue;
        }

        // Termination markers before greedy tokenization so "1-0"
        // is not mistaken for a move number.
        if (auto term = try_consume_termination(c)) {
            out.termination = std::move(*term);
            found_termination = true;
            break;
        }

        if (!is_san_char(ch) && ch != '$') {
            // Unknown punctuation — skip one character to make
            // progress and avoid an infinite loop.
            c.consume();
            continue;
        }

        std::string tok;
        if (ch == '$') {
            tok.push_back(c.consume());
            while (!c.eof() && c.peek() >= '0' && c.peek() <= '9') {
                tok.push_back(c.consume());
            }
        } else {
            tok = read_san_token(c);
            // After a move number, there may be dots still in the
            // buffer (turn marker). Consume any trailing dots here.
            while (!c.eof() && c.peek() == '.') {
                tok.push_back(c.consume());
            }
        }

        if (tok.empty())            continue;
        if (is_move_number(tok))    continue;
        if (is_nag(tok))            continue;

        auto m = parse_san(replay, tok);
        if (!m) {
            return std::unexpected(
                PgnError{"move " + std::to_string(out.moves.size() + 1)
                         + ": " + m.error().message});
        }
        replay.make_move(*m);
        out.moves.push_back(*m);
    }

    if (!found_termination) {
        // Many PGN files in the wild omit the termination marker.
        // Be lenient: accept the parse and mark the game ongoing.
        out.termination = "*";
    }
    return out;
}

// ---------------------------------------------------------------------------
// WRITER
// ---------------------------------------------------------------------------
//
// Tag block:
//   * Emit the STR (Seven-Tag Roster) in the canonical order. A
//     missing STR tag is emitted with "?" as the value so the
//     output file is a valid PGN without the caller having to
//     pre-fill placeholders.
//   * Emit all non-STR tags in the order they appear in `tags`.
//   * Emit `[SetUp "1"]` + `[FEN "…"]` if the game starts from a
//     non-standard position; those replace any existing SetUp/FEN
//     tags in the tag list to avoid duplication.
//
// Move text:
//   * "1. e4 e5 2. Nf3 …", SAN via to_san, one fresh board walk
//     starting from the game's starting_position(). Lines wrapped
//     to a soft 80-column limit (never break inside a token).
//   * If the starting position has black to move, the first move
//     of the first turn becomes "N...". (Rare but valid.)
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] bool is_str_tag(std::string_view k) noexcept {
    for (std::string_view s : PGN_STR_TAGS) {
        if (k == s) return true;
    }
    return false;
}

[[nodiscard]] std::string find_tag(
    const std::vector<std::pair<std::string, std::string>>& tags,
    std::string_view key, std::string_view fallback) {
    for (const auto& kv : tags) {
        if (kv.first == key) return kv.second;
    }
    return std::string{fallback};
}

void emit_tag(std::string& out, std::string_view key, std::string_view value) {
    out.push_back('[');
    out.append(key);
    out.append(" \"");
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    out.append("\"]\n");
}

void append_wrapped(std::string& out, std::string_view token,
                    std::size_t& col) {
    constexpr std::size_t WRAP = 80;
    if (col > 0 && col + 1 + token.size() > WRAP) {
        out.push_back('\n');
        col = 0;
    } else if (col > 0) {
        out.push_back(' ');
        ++col;
    }
    out.append(token);
    col += token.size();
}

} // namespace

std::string write_pgn(
    const Game& game,
    const std::vector<std::pair<std::string, std::string>>& tags,
    std::string_view termination) {
    std::string out;

    // ---- Tag block ------------------------------------------------------
    for (std::string_view str : PGN_STR_TAGS) {
        const std::string v = find_tag(tags, str, "?");
        // The Result tag mirrors the termination.
        if (str == "Result") {
            emit_tag(out, str, termination);
        } else {
            emit_tag(out, str, v);
        }
    }

    const std::string starting_fen = serialize_fen(game.starting_position());
    const bool non_standard_start = starting_fen != std::string{STARTING_POSITION_FEN};

    for (const auto& kv : tags) {
        if (is_str_tag(kv.first)) continue;
        if (kv.first == "SetUp" || kv.first == "FEN") continue; // replaced below
        emit_tag(out, kv.first, kv.second);
    }
    if (non_standard_start) {
        emit_tag(out, "SetUp", "1");
        emit_tag(out, "FEN", starting_fen);
    }
    out.push_back('\n');

    // ---- Move text ------------------------------------------------------
    Board8x8Mailbox board;
    {
        auto r = Board8x8Mailbox::from_fen(starting_fen);
        // Starting FEN came from a live Game, so parse cannot fail.
        board = *r;
    }

    std::size_t col = 0;
    int move_number = board.fullmove_number();
    bool white_to_move = board.side_to_move() == Color::White;

    for (std::size_t i = 0; i < game.moves().size(); ++i) {
        const Move& m = game.moves()[i];

        // Turn indicator. Always emitted before white's move; for
        // black's move it is emitted only if this is the first move
        // of the game (to mark that we skipped white's half).
        if (white_to_move) {
            append_wrapped(out, std::to_string(move_number) + ".", col);
        } else if (i == 0) {
            append_wrapped(out, std::to_string(move_number) + "...", col);
        }

        const std::string san = to_san(board, m);
        append_wrapped(out, san, col);

        board.make_move(m);
        if (!white_to_move) ++move_number;
        white_to_move = !white_to_move;
    }

    append_wrapped(out, std::string{termination}, col);
    out.push_back('\n');
    return out;
}

} // namespace chesserazade

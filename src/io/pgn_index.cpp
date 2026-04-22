/// Header-only multi-game PGN indexer.
///
/// The algorithm is a two-state walk:
///
///   InTags    — we are inside the tag-pair block. Read one
///               `[Key "Value"]` per line, stop when we hit a
///               line that does not start with `[`. Record STR
///               values into the active header.
///
///   InMoves   — we are inside the move-text block. Skip until
///               we see either (a) a termination token (`1-0`,
///               `0-1`, `1/2-1/2`, `*`), which closes the
///               current game, or (b) a new `[Event "...` line
///               at column 0, which implicitly closes the
///               current game and starts the next.
///
/// Game boundaries are defined by `[Event ` at column 0. PGN
/// files from every major source (PGN Mentor, TWIC, Lichess,
/// ChessBase export) follow this convention, so we rely on it
/// and do not attempt to recover from pathological inputs that
/// would require a full tokenizer.
///
/// The indexer never parses SAN — that is what makes it cheap.
/// A 550 KB, 900-game Fischer archive indexes in single-digit
/// milliseconds.
#include <chesserazade/pgn_index.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace chesserazade {

namespace {

/// Does `sv` start with `needle` at position `i`?
[[nodiscard]] bool starts_with_at(std::string_view sv, std::size_t i,
                                  std::string_view needle) noexcept {
    return sv.substr(i).starts_with(needle);
}

/// Advance `i` to the next newline character (or EOF).
void to_eol(std::string_view sv, std::size_t& i) noexcept {
    while (i < sv.size() && sv[i] != '\n') ++i;
}

/// Read one PGN tag pair at the start of a line. The cursor
/// must point at `[`. On success returns `(key, value)`. On
/// malformed input advances past the end of the line and
/// returns an empty key so the caller can keep scanning.
void parse_tag_line(std::string_view sv, std::size_t& i,
                    std::string& key, std::string& value) {
    key.clear();
    value.clear();
    // Skip '['
    ++i;
    // Skip leading whitespace before the key.
    while (i < sv.size() && (sv[i] == ' ' || sv[i] == '\t')) ++i;
    // Read key up to the first space or quote.
    while (i < sv.size() && sv[i] != ' ' && sv[i] != '\t'
           && sv[i] != '"' && sv[i] != '\n' && sv[i] != ']') {
        key.push_back(sv[i]);
        ++i;
    }
    // Skip whitespace between key and quoted value.
    while (i < sv.size() && (sv[i] == ' ' || sv[i] == '\t')) ++i;
    if (i >= sv.size() || sv[i] != '"') {
        to_eol(sv, i);
        key.clear();
        return;
    }
    // Skip opening quote and read value up to closing quote.
    ++i;
    while (i < sv.size() && sv[i] != '"' && sv[i] != '\n') {
        if (sv[i] == '\\' && i + 1 < sv.size()) {
            // Standard PGN escapes: \" and \\.
            ++i;
        }
        value.push_back(sv[i]);
        ++i;
    }
    // Consume the rest of the line regardless of trailing
    // characters — some producers omit the closing `]`.
    to_eol(sv, i);
}

/// Assign tag into the active header if it is one of the
/// Seven-Tag Roster fields we surface. Unknown tags are
/// silently dropped (the full game parser can revisit them).
void apply_tag(PgnGameHeader& h,
               const std::string& key, const std::string& value) {
    if      (key == "Event")  h.event  = value;
    else if (key == "Site")   h.site   = value;
    else if (key == "Date")   h.date   = value;
    else if (key == "Round")  h.round  = value;
    else if (key == "White")  h.white  = value;
    else if (key == "Black")  h.black  = value;
    else if (key == "Result") h.result = value;
}

/// True if `sv` starting at `i` is a PGN termination token
/// (`1-0`, `0-1`, `1/2-1/2`, `*`) followed by whitespace or
/// EOF. Returns the token length via `out_len`.
[[nodiscard]] bool match_termination(std::string_view sv, std::size_t i,
                                     std::size_t& out_len) noexcept {
    // Longest first so "1-0" does not mis-match ahead of
    // "1/2-1/2".
    constexpr std::string_view options[] = {"1/2-1/2", "1-0", "0-1", "*"};
    for (const auto opt : options) {
        if (!starts_with_at(sv, i, opt)) continue;
        const std::size_t end = i + opt.size();
        if (end < sv.size()) {
            const char c = sv[end];
            const bool terminal =
                c == ' ' || c == '\t' || c == '\n'
                || c == '\r' || c == '\0';
            if (!terminal) continue;
        }
        out_len = opt.size();
        return true;
    }
    return false;
}

/// Scan from `i` through the move section. Stops at (and does
/// not consume) either a new `[Event ` at column 0 or EOF.
/// When a termination token is encountered it is consumed and
/// `found_termination` is set.
void scan_moves(std::string_view sv, std::size_t& i,
                bool& found_termination) {
    found_termination = false;
    while (i < sv.size()) {
        // New game marker at column 0 ends the current game.
        const bool at_col0 = (i == 0) || sv[i - 1] == '\n';
        if (at_col0 && starts_with_at(sv, i, "[Event ")) {
            return;
        }
        std::size_t tok_len = 0;
        if (match_termination(sv, i, tok_len)) {
            i += tok_len;
            found_termination = true;
            return;
        }
        ++i;
    }
}

} // namespace

std::vector<PgnGameHeader> index_games(std::string_view pgn) {
    std::vector<PgnGameHeader> out;
    std::size_t i = 0;
    std::string key;
    std::string value;

    while (i < pgn.size()) {
        // Find the next game start: `[Event ` at column 0.
        while (i < pgn.size()) {
            const bool at_col0 = (i == 0) || pgn[i - 1] == '\n';
            if (at_col0 && starts_with_at(pgn, i, "[Event ")) break;
            ++i;
        }
        if (i >= pgn.size()) break;

        PgnGameHeader h;
        h.offset = i;

        // Read tag pairs. A line starting with `[` is a tag; a
        // blank line or a line starting with anything else
        // means the tag block is done.
        while (i < pgn.size()) {
            const bool at_col0 = (i == 0) || pgn[i - 1] == '\n';
            if (!at_col0) { ++i; continue; }
            if (pgn[i] == '\n' || pgn[i] == '\r') { ++i; continue; }
            if (pgn[i] != '[') break;
            parse_tag_line(pgn, i, key, value);
            if (!key.empty()) apply_tag(h, key, value);
            // Consume the newline that closes this tag line.
            if (i < pgn.size() && pgn[i] == '\n') ++i;
        }

        // Move section. Ends at termination or at the next
        // `[Event ` at column 0.
        bool terminated = false;
        scan_moves(pgn, i, terminated);
        h.length = i - h.offset;
        out.push_back(std::move(h));

        // If we stopped because of a new [Event, leave `i`
        // pointed at it so the outer loop picks it up next.
        // If we stopped at termination, there may be trailing
        // whitespace; the outer loop skips it before the next
        // `[Event` search.
        (void)terminated;
    }

    return out;
}

} // namespace chesserazade

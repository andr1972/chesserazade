// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// On-disk PGN index — implementation.
///
/// `build_index` layers over the cheap `index_games` header
/// scan. For each located game we slice the raw PGN bytes,
/// run the full `parse_pgn` parser to recover the move list,
/// replay the moves on a `Board8x8Mailbox` and feed each
/// canonical SAN (check/mate suffix stripped) into an FNV-1a
/// accumulator. The join separator is a single space so two
/// identical move lists hash the same regardless of how the
/// source PGN whitespaced them.
///
/// Games that fail to parse — malformed SAN, illegal replay,
/// missing termination — keep `hash = 0`. They still appear in
/// the index (the list view needs the header row) but are
/// tagged as "hash not available". Later layers should treat
/// hash == 0 as "no cross-file identity".
#include <chesserazade/game_index.hpp>

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/board.hpp>
#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/move_generator.hpp>
#include <chesserazade/pgn.hpp>
#include <chesserazade/san.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace chesserazade {

namespace {

/// FNV-1a 64-bit. Not cryptographic — we only need a stable
/// content-derived key with low collision rate for chess game
/// move lists (typically 40-120 plies, worst case a few
/// hundred); FNV-1a is more than enough and has no library
/// dependency.
constexpr std::uint64_t FNV64_OFFSET = 0xcbf29ce484222325ULL;
constexpr std::uint64_t FNV64_PRIME  = 0x100000001b3ULL;

void fnv_update(std::uint64_t& h, std::string_view s) noexcept {
    for (const char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= FNV64_PRIME;
    }
}

/// Strip check / mate suffix characters from a SAN string.
/// `to_san` emits a single `+` or `#` at the end (never both),
/// but we strip in a loop in case upstream ever changes.
std::string strip_suffix(std::string s) {
    while (!s.empty() && (s.back() == '+' || s.back() == '#')) {
        s.pop_back();
    }
    return s;
}

struct Derived {
    GameHash hash = 0;
    EndKind  end_kind = EndKind::Unknown;
    std::vector<UnderPromotion> underpromotions;
};

[[nodiscard]] char piece_letter(PieceType p) noexcept {
    switch (p) {
        case PieceType::Knight: return 'N';
        case PieceType::Bishop: return 'B';
        case PieceType::Rook:   return 'R';
        case PieceType::Queen:  return 'Q';
        default:                return '?';
    }
}

[[nodiscard]] PieceType piece_from_letter(char c) noexcept {
    switch (c) {
        case 'N': return PieceType::Knight;
        case 'B': return PieceType::Bishop;
        case 'R': return PieceType::Rook;
        case 'Q': return PieceType::Queen;
        default:  return PieceType::None;
    }
}

/// Compute the main-line hash AND the end kind for a game.
/// Single replay pass: we need the running board for canonical
/// SAN anyway, so deriving the terminal state from the final
/// position costs only one extra legal-move generation.
Derived compute_derived(const PgnGame& pg) {
    Derived out;
    if (pg.moves.empty()) return out;

    const std::string_view fen = pg.starting_fen.has_value()
        ? std::string_view{*pg.starting_fen}
        : STARTING_POSITION_FEN;
    auto b = Board8x8Mailbox::from_fen(fen);
    if (!b) return out;
    Board8x8Mailbox board = *b;

    std::uint64_t h = FNV64_OFFSET;
    bool first = true;
    int ply = 0;
    for (const auto& m : pg.moves) {
        ++ply;
        std::string san = strip_suffix(to_san(board, m));
        if (!first) fnv_update(h, " ");
        fnv_update(h, san);
        first = false;

        // Underpromotion: any promotion whose target piece is
        // not a queen. En passant and regular captures are not
        // promotions; PromotionCapture carries `promotion`
        // just like plain Promotion.
        if ((m.kind == MoveKind::Promotion
             || m.kind == MoveKind::PromotionCapture)
            && m.promotion != PieceType::Queen) {
            out.underpromotions.push_back({ply, m.promotion});
        }

        board.make_move(m);
    }
    out.hash = h;

    const MoveList legal = MoveGenerator::generate_legal(board);
    if (legal.empty()) {
        if (MoveGenerator::is_in_check(board, board.side_to_move())) {
            out.end_kind = EndKind::Mate;
        } else {
            out.end_kind = EndKind::Stalemate;
        }
    } else {
        out.end_kind = EndKind::Other;
    }
    return out;
}

[[nodiscard]] std::string_view end_kind_name(EndKind k) noexcept {
    switch (k) {
        case EndKind::Mate:      return "mate";
        case EndKind::Stalemate: return "stalemate";
        case EndKind::Other:     return "other";
        case EndKind::Unknown:   return "unknown";
    }
    return "unknown";
}

[[nodiscard]] EndKind end_kind_from_name(std::string_view s) noexcept {
    if (s == "mate")      return EndKind::Mate;
    if (s == "stalemate") return EndKind::Stalemate;
    if (s == "other")     return EndKind::Other;
    return EndKind::Unknown;
}

/// Minimal JSON writer. PGN tag values can contain arbitrary
/// bytes; escape `"` and `\` plus the control characters that
/// must be escaped per RFC 8259. Everything else (UTF-8
/// continuation bytes, high-bit Latin-1) is passed through.
void write_json_string(std::ostream& os, std::string_view s) {
    os << '"';
    for (const char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b";  break;
            case '\f': os << "\\f";  break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf),
                                  "\\u%04x",
                                  static_cast<unsigned>(
                                      static_cast<unsigned char>(c)));
                    os << buf;
                } else {
                    os << c;
                }
                break;
        }
    }
    os << '"';
}

void write_record(std::ostream& os, const GameRecord& r) {
    const auto& h = r.header;
    os << "    {\n";
    os << "      \"event\": ";  write_json_string(os, h.event);  os << ",\n";
    os << "      \"site\": ";   write_json_string(os, h.site);   os << ",\n";
    os << "      \"date\": ";   write_json_string(os, h.date);   os << ",\n";
    os << "      \"round\": ";  write_json_string(os, h.round);  os << ",\n";
    os << "      \"white\": ";  write_json_string(os, h.white);  os << ",\n";
    os << "      \"black\": ";  write_json_string(os, h.black);  os << ",\n";
    os << "      \"result\": "; write_json_string(os, h.result); os << ",\n";
    os << "      \"eco\": ";    write_json_string(os, h.eco);    os << ",\n";
    os << "      \"ply_count\": " << h.ply_count               << ",\n";
    os << "      \"offset\": "    << h.offset                  << ",\n";
    os << "      \"length\": "    << h.length                  << ",\n";
    os << "      \"hash\": \""    << std::hex << r.hash << std::dec
       << "\",\n";
    os << "      \"end_kind\": ";
    write_json_string(os, end_kind_name(r.end_kind));
    os << ",\n";
    os << "      \"underpromotions\": [";
    for (std::size_t i = 0; i < r.underpromotions.size(); ++i) {
        const auto& up = r.underpromotions[i];
        if (i > 0) os << ", ";
        os << "{\"ply\": " << up.ply
           << ", \"piece\": \"" << piece_letter(up.piece) << "\"}";
    }
    os << "]\n";
    os << "    }";
}

// --- Minimal JSON reader for the loader side ---------------------------------
// We only parse the shape we write, not arbitrary JSON. A full parser would
// be overkill for a single-object-with-one-array schema.

struct Cursor {
    std::string_view s;
    std::size_t      i = 0;

    bool eof() const noexcept { return i >= s.size(); }
    char peek() const noexcept { return eof() ? '\0' : s[i]; }
    void skip_ws() noexcept {
        while (!eof() && (s[i] == ' ' || s[i] == '\t'
                        || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool match(char c) noexcept {
        skip_ws();
        if (peek() != c) return false;
        ++i;
        return true;
    }
    bool match_literal(std::string_view lit) noexcept {
        skip_ws();
        if (s.substr(i).starts_with(lit)) {
            i += lit.size();
            return true;
        }
        return false;
    }
};

bool parse_string(Cursor& c, std::string& out) {
    c.skip_ws();
    if (c.peek() != '"') return false;
    ++c.i;
    out.clear();
    while (!c.eof() && c.s[c.i] != '"') {
        char ch = c.s[c.i++];
        if (ch == '\\' && !c.eof()) {
            const char e = c.s[c.i++];
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (c.i + 4 > c.s.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = c.s[c.i++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9')      cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(10 + h - 'a');
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(10 + h - 'A');
                        else return false;
                    }
                    // Only encode ASCII-range here; we never
                    // emit non-ASCII \u so a round-trip works.
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    break;
                }
                default: return false;
            }
        } else {
            out.push_back(ch);
        }
    }
    if (c.eof()) return false;
    ++c.i; // closing "
    return true;
}

bool parse_int(Cursor& c, long long& out) {
    c.skip_ws();
    const std::size_t start = c.i;
    if (!c.eof() && (c.s[c.i] == '-' || c.s[c.i] == '+')) ++c.i;
    while (!c.eof() && c.s[c.i] >= '0' && c.s[c.i] <= '9') ++c.i;
    if (c.i == start) return false;
    const std::string chunk{c.s.substr(start, c.i - start)};
    try { out = std::stoll(chunk); } catch (...) { return false; }
    return true;
}

bool parse_hash(Cursor& c, std::uint64_t& out) {
    std::string s;
    if (!parse_string(c, s)) return false;
    try { out = std::stoull(s, nullptr, 16); } catch (...) { return false; }
    return true;
}

bool expect_key(Cursor& c, std::string_view k) {
    std::string key;
    if (!parse_string(c, key)) return false;
    if (key != k) return false;
    return c.match(':');
}

bool parse_record(Cursor& c, GameRecord& r) {
    if (!c.match('{')) return false;
    auto& h = r.header;
    long long i64 = 0;

    const auto str_field = [&](std::string_view k, std::string& dst) {
        if (!expect_key(c, k)) return false;
        if (!parse_string(c, dst)) return false;
        return c.match(',');
    };
    const auto int_field = [&](std::string_view k, long long& dst,
                               bool trailing_comma) {
        if (!expect_key(c, k)) return false;
        if (!parse_int(c, dst)) return false;
        if (trailing_comma) return c.match(',');
        return true;
    };

    if (!str_field("event",   h.event))  return false;
    if (!str_field("site",    h.site))   return false;
    if (!str_field("date",    h.date))   return false;
    if (!str_field("round",   h.round))  return false;
    if (!str_field("white",   h.white))  return false;
    if (!str_field("black",   h.black))  return false;
    if (!str_field("result",  h.result)) return false;
    if (!str_field("eco",     h.eco))    return false;
    if (!int_field("ply_count", i64, true)) return false;
    h.ply_count = static_cast<int>(i64);
    if (!int_field("offset", i64, true)) return false;
    h.offset = static_cast<std::size_t>(i64);
    if (!int_field("length", i64, true)) return false;
    h.length = static_cast<std::size_t>(i64);
    if (!expect_key(c, "hash")) return false;
    if (!parse_hash(c, r.hash)) return false;
    if (!c.match(',')) return false;
    if (!expect_key(c, "end_kind")) return false;
    std::string ek;
    if (!parse_string(c, ek)) return false;
    r.end_kind = end_kind_from_name(ek);
    if (!c.match(',')) return false;
    if (!expect_key(c, "underpromotions")) return false;
    if (!c.match('[')) return false;
    c.skip_ws();
    if (c.peek() != ']') {
        while (true) {
            if (!c.match('{')) return false;
            if (!expect_key(c, "ply")) return false;
            long long p = 0;
            if (!parse_int(c, p)) return false;
            if (!c.match(',')) return false;
            if (!expect_key(c, "piece")) return false;
            std::string pc;
            if (!parse_string(c, pc)) return false;
            if (!c.match('}')) return false;
            UnderPromotion up;
            up.ply   = static_cast<int>(p);
            up.piece = pc.empty() ? PieceType::None
                                  : piece_from_letter(pc[0]);
            r.underpromotions.push_back(up);
            if (c.match(',')) continue;
            break;
        }
    }
    if (!c.match(']')) return false;
    if (!c.match('}')) return false;
    return true;
}

} // namespace

GameIndex build_index(std::string_view pgn_bytes,
                      std::int64_t pgn_mtime,
                      const BuildProgressCb& progress,
                      const std::atomic<bool>& cancel) {
    GameIndex idx;
    idx.schema = 3;
    idx.pgn_mtime = pgn_mtime;

    const auto headers = index_games(pgn_bytes);
    const std::size_t total = headers.size();
    idx.games.reserve(total);

    for (std::size_t gi = 0; gi < total; ++gi) {
        if (cancel.load(std::memory_order_relaxed)) break;

        GameRecord r;
        r.header = headers[gi];

        // Slice the game text and run the full parser. A parse
        // failure is recorded as hash=0 and the record is kept.
        const std::string_view slice =
            pgn_bytes.substr(r.header.offset, r.header.length);
        const auto parsed = parse_pgn(slice);
        if (parsed.has_value()) {
            Derived d = compute_derived(*parsed);
            r.hash = d.hash;
            r.end_kind = d.end_kind;
            r.underpromotions = std::move(d.underpromotions);
        }
        idx.games.push_back(std::move(r));

        if (progress && ((gi + 1) % 50 == 0 || gi + 1 == total)) {
            progress(gi + 1, total);
        }
    }
    return idx;
}

bool save_index(const std::string& path, const GameIndex& idx) {
    std::ofstream out(path);
    if (!out) return false;

    out << "{\n";
    out << "  \"schema\": "    << idx.schema    << ",\n";
    out << "  \"pgn_mtime\": " << idx.pgn_mtime << ",\n";
    out << "  \"games\": [\n";
    for (std::size_t i = 0; i < idx.games.size(); ++i) {
        write_record(out, idx.games[i]);
        if (i + 1 < idx.games.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return static_cast<bool>(out);
}

std::optional<GameIndex> load_index(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    Cursor c{text};
    GameIndex idx;

    if (!c.match('{')) return std::nullopt;
    long long i64 = 0;
    if (!expect_key(c, "schema") || !parse_int(c, i64)
        || !c.match(',')) return std::nullopt;
    idx.schema = static_cast<int>(i64);
    // Only accept the current schema. Older indexes lack
    // fields we need (end_kind in v2, underpromotions in v3);
    // rather than migrate we return nullopt so the caller
    // rebuilds from the PGN — rebuild is cheap and keeps the
    // loader free of per-version fixup code.
    if (idx.schema != 3) return std::nullopt;

    if (!expect_key(c, "pgn_mtime") || !parse_int(c, i64)
        || !c.match(',')) return std::nullopt;
    idx.pgn_mtime = static_cast<std::int64_t>(i64);

    if (!expect_key(c, "games") || !c.match('[')) return std::nullopt;
    c.skip_ws();
    if (c.peek() != ']') {
        while (true) {
            GameRecord r;
            if (!parse_record(c, r)) return std::nullopt;
            idx.games.push_back(std::move(r));
            if (c.match(',')) continue;
            break;
        }
    }
    if (!c.match(']')) return std::nullopt;
    if (!c.match('}')) return std::nullopt;
    return idx;
}

} // namespace chesserazade

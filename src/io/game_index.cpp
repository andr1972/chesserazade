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

#include <chesserazade/bitboard.hpp>
#include <chesserazade/board.hpp>
#include <chesserazade/evaluator.hpp>
#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/move_generator.hpp>
#include <chesserazade/pgn.hpp>
#include <chesserazade/san.hpp>
#include <chesserazade/types.hpp>

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
    std::vector<int> knight_fork_plies;
    std::vector<MaterialSac> material_sacs;
};

/// Net material balance from white's perspective, in cp.
/// Positive = white ahead. Kings excluded.
[[nodiscard]] int material_balance(const Board& b) noexcept {
    int bal = 0;
    for (int i = 0; i < 64; ++i) {
        const Square sq = static_cast<Square>(i);
        const Piece p = b.piece_at(sq);
        if (p.type == PieceType::None
            || p.type == PieceType::King) continue;
        const int v = piece_value(p.type);
        bal += (p.color == Color::White) ? v : -v;
    }
    return bal;
}

/// Threshold below which a small group's signed sum is
/// considered "noise" (knight-for-bishop nets to ±10,
/// pawn-for-pawn to 0). Such small groups are dropped from
/// the sign sequence before episodes are partitioned.
/// Strict less-than: a single unrecaptured pawn (sum = ±100)
/// passes and contributes to the next large group.
constexpr int SMALL_GROUP_DROP_BELOW = 100;

/// Find sacrifice events.
///
/// Two-ply "mover's move + opponent's reply" gives a wrong
/// answer when a sacrifice unfolds across an exchange tower:
/// Fischer's 17…Be6!! drops the queen only on ply 35, and a
/// fixed 2-ply window starting from Fischer's move nets the
/// immediate bishop-recapture, reporting rook-magnitude
/// instead of queen. Fix: **dynamically extend** the window
/// through any contiguous chain of captures, up to
/// `EXCHANGE_MAX_WINDOW` plies, then measure the net once the
/// exchange has stabilised. This matches user intuition:
///     +500 −500        → net 0      → skipped (plain trade)
///     +500 −500 +300   → net 300    → sacrifice
///     +500 −500 +300 −300 → net 0   → skipped (full cycle)
///
/// Gate: net_loss ≥ NET_THRESHOLD_CP (300 cp). No separate
/// raw-loss gate — raw is recorded for display but doesn't
/// suppress events.
///
/// Events for different mover-plies can overlap; the UI picks
/// the largest by raw_loss for the Sac column, and the
/// filter-by-sacrifice checkbox just tests non-empty.
// One "small group": consecutive captures uninterrupted by a
// quiet ply. Stores its signed sum (white-perspective) and
// the biggest piece that fell in the run — used to filter
// pawn-only large groups later.
struct SmallGroup {
    int       start_ply = 0;   // 1-based, ply of first capture
    int       sum_cp = 0;      // signed; positive = white gained
    PieceType max_piece = PieceType::None;
    int       max_piece_cp = 0;
};

[[nodiscard]] std::vector<SmallGroup>
find_small_groups(const std::vector<int>& balances,
                  const std::vector<Move>& moves) {
    std::vector<SmallGroup> out;
    const int n = static_cast<int>(moves.size());
    int i = 0;
    while (i < n) {
        const int delta = balances[static_cast<std::size_t>(i + 1)]
                        - balances[static_cast<std::size_t>(i)];
        if (delta == 0) { ++i; continue; }

        SmallGroup g;
        g.start_ply = i + 1; // 1-based
        // Walk forward across consecutive captures.
        int j = i;
        while (j < n) {
            const int d = balances[static_cast<std::size_t>(j + 1)]
                        - balances[static_cast<std::size_t>(j)];
            if (d == 0) break;
            g.sum_cp += d;
            const PieceType cap =
                moves[static_cast<std::size_t>(j)].captured_piece.type;
            if (cap != PieceType::None) {
                const int v = piece_value(cap);
                if (v > g.max_piece_cp) {
                    g.max_piece_cp = v;
                    g.max_piece = cap;
                }
            }
            ++j;
        }
        out.push_back(g);
        i = j;
    }
    return out;
}

// Sign label for a small group. 0 marks groups whose sum is
// below the noise threshold — they're dropped from the
// sequence before episode partitioning.
[[nodiscard]] int sign_of(const SmallGroup& g) noexcept {
    if (g.sum_cp >= SMALL_GROUP_DROP_BELOW)  return +1;
    if (g.sum_cp <= -SMALL_GROUP_DROP_BELOW) return -1;
    return 0;
}

// Partition a stream of (already-sign-filtered, non-zero)
// small-group indices into "episodes" for one perspective.
// `leading_sign` selects the perspective:
//   +1 → white's view, episodes shaped (+...+−...−)
//   −1 → black's view, episodes shaped (−...−+...+)
// Each returned vector is the contiguous list of small-group
// indices belonging to one episode. Episodes that never see
// the trailing sign (e.g. a final hanging `+` for white) are
// kept as "open" — net is just the lead-side sum.
[[nodiscard]] std::vector<std::vector<int>>
partition_episodes(const std::vector<SmallGroup>& groups,
                   const std::vector<int>& nonzero_indices,
                   int leading_sign) {
    std::vector<std::vector<int>> episodes;
    enum class Phase { Pre, Lead, Trail };
    Phase phase = Phase::Pre;
    std::vector<int> current;
    for (const int gi : nonzero_indices) {
        const int s = sign_of(groups[static_cast<std::size_t>(gi)]);
        switch (phase) {
            case Phase::Pre:
                if (s == leading_sign) {
                    current.push_back(gi);
                    phase = Phase::Lead;
                }
                // else skip leading opposite-sign small groups
                break;
            case Phase::Lead:
                if (s == leading_sign) {
                    current.push_back(gi);
                } else {
                    current.push_back(gi);
                    phase = Phase::Trail;
                }
                break;
            case Phase::Trail:
                if (s == leading_sign) {
                    // Start of new episode.
                    episodes.push_back(std::move(current));
                    current.clear();
                    current.push_back(gi);
                    phase = Phase::Lead;
                } else {
                    current.push_back(gi);
                }
                break;
        }
    }
    if (!current.empty()) episodes.push_back(std::move(current));
    return episodes;
}

std::vector<MaterialSac> find_sacs(const std::vector<int>& balances,
                                   const std::vector<Move>& moves) {
    std::vector<MaterialSac> out;
    if (moves.empty()) return out;

    const auto small_groups = find_small_groups(balances, moves);

    // Index list of small groups whose sign != 0.
    std::vector<int> nonzero;
    nonzero.reserve(small_groups.size());
    for (std::size_t i = 0; i < small_groups.size(); ++i) {
        if (sign_of(small_groups[i]) != 0) {
            nonzero.push_back(static_cast<int>(i));
        }
    }
    if (nonzero.empty()) return out;

    // Partition for both perspectives and emit episodes.
    for (const int leading : {+1, -1}) {
        const auto episodes =
            partition_episodes(small_groups, nonzero, leading);
        for (const auto& ep : episodes) {
            if (ep.empty()) continue;
            // Filter: drop episodes whose every small group has
            // max piece ≤ pawn — banal pawn-only exchanges.
            bool any_above_pawn = false;
            for (const int gi : ep) {
                const auto& g = small_groups[static_cast<std::size_t>(gi)];
                if (g.max_piece_cp > piece_value(PieceType::Pawn)) {
                    any_above_pawn = true;
                    break;
                }
            }
            if (!any_above_pawn) continue;

            // Episode metrics.
            int net_white = 0;
            PieceType raw_piece = PieceType::None;
            int raw_piece_cp = 0;
            for (const int gi : ep) {
                const auto& g = small_groups[static_cast<std::size_t>(gi)];
                net_white += g.sum_cp;
                if (g.max_piece_cp > raw_piece_cp) {
                    raw_piece_cp = g.max_piece_cp;
                    raw_piece = g.max_piece;
                }
            }
            // Owner-perspective net: positive = owner gained,
            // negative = owner sacrificed-without-recovery.
            const int net_owner = (leading == +1) ? net_white : -net_white;

            MaterialSac s;
            s.ply = small_groups[static_cast<std::size_t>(ep.front())]
                        .start_ply;
            s.owner = (leading == +1) ? Color::White : Color::Black;
            s.net_cp = net_owner;
            s.raw_piece = raw_piece;
            s.raw_piece_cp = raw_piece_cp;
            out.push_back(s);
        }
    }
    return out;
}

/// True iff a knight of `mover` sitting on `sq` in `board`
/// both checks the opposing king and attacks at least one
/// opposing queen or rook on a separate square. Called
/// *after* `make_move`, so `board.side_to_move()` is the
/// opponent and the knight is the piece the opponent is
/// threatened by.
[[nodiscard]] bool detect_knight_fork(const Board& board,
                                      Color mover,
                                      Square knight_sq) noexcept {
    const Color them = (mover == Color::White) ? Color::Black
                                               : Color::White;
    // 1. Must give check.
    if (!MoveGenerator::is_in_check(board, them)) return false;

    // 2. Among the knight's attacked squares, find at least one
    // opposing Q or R.
    Bitboard attacked = Attacks::knight(knight_sq);
    while (attacked) {
        const Square s = pop_lsb(attacked);
        const Piece p = board.piece_at(s);
        if (p.type == PieceType::Queen || p.type == PieceType::Rook) {
            if (p.color == them) return true;
        }
    }
    return false;
}

[[nodiscard]] char piece_letter(PieceType p) noexcept {
    switch (p) {
        case PieceType::Pawn:   return 'P';
        case PieceType::Knight: return 'N';
        case PieceType::Bishop: return 'B';
        case PieceType::Rook:   return 'R';
        case PieceType::Queen:  return 'Q';
        case PieceType::King:   return 'K';
        case PieceType::None:   return '-';
    }
    return '-';
}

[[nodiscard]] PieceType piece_from_letter(char c) noexcept {
    switch (c) {
        case 'P': return PieceType::Pawn;
        case 'N': return PieceType::Knight;
        case 'B': return PieceType::Bishop;
        case 'R': return PieceType::Rook;
        case 'Q': return PieceType::Queen;
        case 'K': return PieceType::King;
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

    std::vector<int> balances;
    balances.reserve(pg.moves.size() + 1);
    balances.push_back(material_balance(board));

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

        const Color mover = board.side_to_move();
        board.make_move(m);

        // Knight fork: the piece now sitting on `m.to` is a
        // knight (either a knight move or a promotion to a
        // knight) and it checks + attacks Q/R.
        const Piece landed = board.piece_at(m.to);
        if (landed.type == PieceType::Knight
            && landed.color == mover
            && detect_knight_fork(board, mover, m.to)) {
            out.knight_fork_plies.push_back(ply);
        }

        balances.push_back(material_balance(board));
    }
    out.hash = h;
    out.material_sacs = find_sacs(balances, pg.moves);

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
    os << "],\n";
    os << "      \"knight_fork_plies\": [";
    for (std::size_t i = 0; i < r.knight_fork_plies.size(); ++i) {
        if (i > 0) os << ", ";
        os << r.knight_fork_plies[i];
    }
    os << "],\n";
    os << "      \"material_sacs\": [";
    for (std::size_t i = 0; i < r.material_sacs.size(); ++i) {
        const auto& s = r.material_sacs[i];
        if (i > 0) os << ", ";
        os << "{\"ply\": " << s.ply
           << ", \"owner\": \""
           << (s.owner == Color::White ? "white" : "black") << "\""
           << ", \"net_cp\": " << s.net_cp
           << ", \"raw_piece\": \"" << piece_letter(s.raw_piece) << "\""
           << ", \"raw_piece_cp\": " << s.raw_piece_cp << "}";
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
    if (!c.match(',')) return false;
    if (!expect_key(c, "knight_fork_plies")) return false;
    if (!c.match('[')) return false;
    c.skip_ws();
    if (c.peek() != ']') {
        while (true) {
            long long p = 0;
            if (!parse_int(c, p)) return false;
            r.knight_fork_plies.push_back(static_cast<int>(p));
            if (c.match(',')) continue;
            break;
        }
    }
    if (!c.match(']')) return false;
    if (!c.match(',')) return false;
    if (!expect_key(c, "material_sacs")) return false;
    if (!c.match('[')) return false;
    c.skip_ws();
    if (c.peek() != ']') {
        while (true) {
            if (!c.match('{')) return false;
            MaterialSac s;
            long long v = 0;
            if (!expect_key(c, "ply") || !parse_int(c, v)) return false;
            s.ply = static_cast<int>(v);
            if (!c.match(',')) return false;
            if (!expect_key(c, "owner")) return false;
            std::string ow;
            if (!parse_string(c, ow)) return false;
            s.owner = (ow == "black") ? Color::Black : Color::White;
            if (!c.match(',')) return false;
            if (!expect_key(c, "net_cp") || !parse_int(c, v)) return false;
            s.net_cp = static_cast<int>(v);
            if (!c.match(',')) return false;
            if (!expect_key(c, "raw_piece")) return false;
            std::string rp;
            if (!parse_string(c, rp)) return false;
            s.raw_piece = rp.empty() ? PieceType::None
                                     : piece_from_letter(rp[0]);
            if (!c.match(',')) return false;
            if (!expect_key(c, "raw_piece_cp")
                || !parse_int(c, v)) return false;
            s.raw_piece_cp = static_cast<int>(v);
            if (!c.match('}')) return false;
            r.material_sacs.push_back(s);
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
    idx.schema = 9;
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
            r.knight_fork_plies = std::move(d.knight_fork_plies);
            r.material_sacs = std::move(d.material_sacs);
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
    if (idx.schema != 9) return std::nullopt;

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

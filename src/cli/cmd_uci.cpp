// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// UCI mode.
///
/// Implements a single-threaded, blocking UCI loop. The search
/// runs synchronously inside the handler for `go`, so `stop` and
/// `ponderhit` are intentionally not supported in 1.2 — the
/// engine respects its own time / node budget and then sends
/// `bestmove`. Multi-threading + `stop` are deferred to a later
/// version.
///
/// Per-depth `info` output is produced by driving iterative
/// deepening from here: we call `Search::find_best` once per
/// target depth `d = 1..max`, share the `TranspositionTable`
/// across calls, and print one `info` line per completed
/// iteration. Internally `find_best` re-iterates 1..d every time,
/// but the TT makes shallow re-runs almost free, and the code
/// stays simple — no callback plumbing into the engine core.
///
/// See `TODO.md` §1.2 for the protocol subset; reference at
/// https://wbec-ridderkerk.nl/html/UCIProtocol.html.
#include "cli/cmd_uci.hpp"

#include "board/board8x8_mailbox.hpp"
#include "board/board_bitboard.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/move_generator.hpp>
#include <chesserazade/search.hpp>
#include <chesserazade/transposition_table.hpp>
#include <chesserazade/types.hpp>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace chesserazade::cli {

namespace {

constexpr std::string_view ENGINE_NAME    = "chesserazade";
constexpr std::string_view ENGINE_VERSION = "1.2.0";
constexpr std::string_view ENGINE_AUTHOR  = "Andrzej Borucki";

/// Default TT size in MiB. One `TtEntry` is 16 bytes, so 16 MiB
/// ≈ 1 Mi entries — matches the historic default of `cmd_solve`.
constexpr int DEFAULT_HASH_MB = 16;
constexpr int MIN_HASH_MB = 1;
constexpr int MAX_HASH_MB = 4096;

/// Number of `TtEntry` slots for a given `mb`. `TranspositionTable`
/// rounds down to a power of two internally, so the resulting
/// size is <= requested.
[[nodiscard]] std::size_t entries_for_mb(int mb) noexcept {
    const std::size_t bytes = static_cast<std::size_t>(mb) * 1024u * 1024u;
    return bytes / sizeof(TtEntry);
}

// ---------------------------------------------------------------------------
// Tokenization
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    for (std::string tok; is >> tok; ) out.push_back(std::move(tok));
    return out;
}

[[nodiscard]] std::optional<long long> to_int(const std::string& s) noexcept {
    long long n = 0;
    const auto* b = s.data();
    const auto* e = b + s.size();
    const auto [_, ec] = std::from_chars(b, e, n);
    if (ec != std::errc{}) return std::nullopt;
    return n;
}

// ---------------------------------------------------------------------------
// Move resolution (UCI long-algebraic -> Move)
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<Move> resolve_uci(Board& board,
                                              std::string_view s) {
    if (s.size() != 4 && s.size() != 5) return std::nullopt;
    const auto from = square_from_algebraic(s.substr(0, 2));
    const auto to   = square_from_algebraic(s.substr(2, 2));
    if (!from || !to) return std::nullopt;

    std::optional<PieceType> promo;
    if (s.size() == 5) {
        switch (s[4]) {
            case 'q': promo = PieceType::Queen;  break;
            case 'r': promo = PieceType::Rook;   break;
            case 'b': promo = PieceType::Bishop; break;
            case 'n': promo = PieceType::Knight; break;
            default: return std::nullopt;
        }
    }

    const MoveList legal = MoveGenerator::generate_legal(board);
    for (const Move& m : legal) {
        if (m.from != *from || m.to != *to) continue;
        if (promo && m.promotion != *promo) continue;
        if (!promo && (m.kind == MoveKind::Promotion
                       || m.kind == MoveKind::PromotionCapture)) {
            continue;
        }
        return m;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

void handle_uci(std::ostream& out) {
    out << "id name " << ENGINE_NAME << ' ' << ENGINE_VERSION << '\n';
    out << "id author " << ENGINE_AUTHOR << '\n';
    out << "option name Hash type spin default " << DEFAULT_HASH_MB
        << " min " << MIN_HASH_MB << " max " << MAX_HASH_MB << '\n';
    out << "option name Threads type spin default 1 min 1 max 1\n";
    out << "uciok\n" << std::flush;
}

void handle_isready(std::ostream& out) {
    out << "readyok\n" << std::flush;
}

void handle_ucinewgame(UciSession& s) {
    s.tt.clear();
    auto r = Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    if (r) s.board = *r;
    s.position_history.clear();
}

/// `setoption name <X> [value <Y>]`. We accept the documented
/// options and silently ignore unknowns — GUIs sometimes probe.
void handle_setoption(UciSession& s,
                      const std::vector<std::string>& toks) {
    // Format: setoption name <N...> value <V...>
    std::size_t i = 1;
    if (i >= toks.size() || toks[i] != "name") return;
    ++i;
    std::string name;
    while (i < toks.size() && toks[i] != "value") {
        if (!name.empty()) name.push_back(' ');
        name += toks[i];
        ++i;
    }
    std::string value;
    if (i < toks.size() && toks[i] == "value") {
        ++i;
        while (i < toks.size()) {
            if (!value.empty()) value.push_back(' ');
            value += toks[i];
            ++i;
        }
    }

    if (name == "Hash") {
        if (auto v = to_int(value)) {
            int mb = static_cast<int>(*v);
            if (mb < MIN_HASH_MB) mb = MIN_HASH_MB;
            if (mb > MAX_HASH_MB) mb = MAX_HASH_MB;
            s.hash_mb = mb;
            s.tt = TranspositionTable{entries_for_mb(mb)};
        }
    }
    // "Threads" is accepted but no-op (single-threaded engine).
}

/// `position {startpos | fen <fen>} [moves m1 m2 ...]`.
void handle_position(UciSession& s,
                     const std::vector<std::string>& toks) {
    std::size_t i = 1;
    if (i >= toks.size()) return;

    std::string fen;
    if (toks[i] == "startpos") {
        fen = std::string{STARTING_POSITION_FEN};
        ++i;
    } else if (toks[i] == "fen") {
        ++i;
        // FEN is six space-separated fields. Collect until "moves"
        // or end-of-tokens.
        std::vector<std::string> fields;
        while (i < toks.size() && toks[i] != "moves" && fields.size() < 6) {
            fields.push_back(toks[i]);
            ++i;
        }
        for (std::size_t k = 0; k < fields.size(); ++k) {
            if (k) fen.push_back(' ');
            fen += fields[k];
        }
    } else {
        return;
    }

    auto r = Board8x8Mailbox::from_fen(fen);
    if (!r) return;
    s.board = *r;

    // Rebuild position history from scratch on every `position`
    // command — UCI lets the GUI overwrite the game tree at any
    // point. Push every position *before* the live `s.board`
    // (i.e. the start FEN, then the position after each played
    // move except the last); the live position itself goes onto
    // the search-path stack via the root negamax frame.
    s.position_history.clear();
    s.position_history.push_back(s.board.zobrist_key());

    if (i < toks.size() && toks[i] == "moves") {
        ++i;
        for (; i < toks.size(); ++i) {
            auto m = resolve_uci(s.board, toks[i]);
            if (!m) return;
            s.board.make_move(*m);
            s.position_history.push_back(s.board.zobrist_key());
        }
    }
    // Drop the live position — it'll appear on the search path
    // as the root, and game-history entries equal to the root
    // would otherwise look like a self-repetition at every node.
    if (!s.position_history.empty()) {
        s.position_history.pop_back();
    }
}

// ---------------------------------------------------------------------------
// `go` — time management + iterative deepening with info output
// ---------------------------------------------------------------------------

struct GoParams {
    int depth = -1;
    long long movetime_ms = 0;
    long long wtime = 0;
    long long btime = 0;
    long long winc = 0;
    long long binc = 0;
    int movestogo = 0;
    bool infinite = false;
};

[[nodiscard]] GoParams parse_go(const std::vector<std::string>& toks) {
    GoParams g;
    for (std::size_t i = 1; i < toks.size(); ++i) {
        const std::string& t = toks[i];
        if (t == "infinite") { g.infinite = true; continue; }
        if (i + 1 >= toks.size()) continue;
        const auto v = to_int(toks[i + 1]);
        if (!v) continue;
        if      (t == "depth")     g.depth = static_cast<int>(*v);
        else if (t == "movetime")  g.movetime_ms = *v;
        else if (t == "wtime")     g.wtime = *v;
        else if (t == "btime")     g.btime = *v;
        else if (t == "winc")      g.winc = *v;
        else if (t == "binc")      g.binc = *v;
        else if (t == "movestogo") g.movestogo = static_cast<int>(*v);
        else if (t == "nodes")     { /* accepted, unused */ }
        else continue;
        ++i;
    }
    return g;
}

/// Classical time-budget formula: `time / divisor + inc / 2`.
/// `movestogo` shortens the divisor when the GUI has told us how
/// many moves remain until the next time control. We keep a safety
/// margin so the move arrives before the flag falls.
[[nodiscard]] long long derive_movetime(const GoParams& g, Color stm) {
    const long long time = (stm == Color::White) ? g.wtime : g.btime;
    const long long inc  = (stm == Color::White) ? g.winc  : g.binc;
    if (time <= 0 && inc <= 0) return 0;

    const int divisor = g.movestogo > 0 ? (g.movestogo + 2) : 30;
    long long ms = time / divisor + inc / 2;

    // Never spend more than half of the remaining budget on a
    // single move, and keep a 50 ms safety margin below the flag.
    const long long cap = time > 100 ? (time / 2 - 50) : 0;
    if (cap > 0 && ms > cap) ms = cap;
    if (ms < 1) ms = 1;
    return ms;
}

[[nodiscard]] std::string format_score(int score) {
    if (Search::is_mate_score(score)) {
        const int plies = Search::plies_to_mate(score);
        const int moves = (plies >= 0 ? plies + 1 : plies - 1) / 2;
        return "mate " + std::to_string(moves);
    }
    return "cp " + std::to_string(score);
}

[[nodiscard]] std::string format_pv(const std::vector<Move>& pv) {
    std::string s;
    for (std::size_t i = 0; i < pv.size(); ++i) {
        if (i) s.push_back(' ');
        s += to_uci(pv[i]);
    }
    return s;
}

void emit_info(const SearchResult& r, std::ostream& out) {
    const long long ms = r.elapsed.count() > 0 ? r.elapsed.count() : 1;
    const std::uint64_t nps =
        static_cast<std::uint64_t>(
            static_cast<double>(r.nodes) * 1000.0
            / static_cast<double>(ms));
    out << "info depth " << r.completed_depth
        << " score " << format_score(r.score)
        << " nodes " << r.nodes
        << " nps " << nps
        << " time " << ms;
    if (!r.principal_variation.empty()) {
        out << " pv " << format_pv(r.principal_variation);
    }
    out << '\n';
    // NMP diagnostics — `info string` is a free-form UCI line that
    // GUIs and match.py treat as a comment. Skipped when nothing
    // entered the gate so untouched searches stay quiet.
    if (r.nmp_entered + r.nmp_rejected > 0) {
        out << "info string nmp"
            << " rejected=" << r.nmp_rejected
            << " entered=" << r.nmp_entered
            << " failed_high=" << r.nmp_failed_high
            << " verify_attempts=" << r.nmp_verify_attempts
            << " verified=" << r.nmp_verified
            << " aborted=" << r.nmp_aborted
            << '\n';
    }
    out << std::flush;
}

void handle_go(UciSession& s, const std::vector<std::string>& toks,
               std::ostream& out) {
    const GoParams g = parse_go(toks);

    int max_depth = Search::MAX_DEPTH;
    long long budget_ms = 0;

    if (g.infinite) {
        // No time cap; rely on MAX_DEPTH.
    } else {
        // Allow both `depth` and `movetime` to fire — the search
        // stops at whichever limit hits first. Lets a tester
        // force "1000 ms but at most 14 ply" with
        // `go depth 14 movetime 1000`.
        if (g.depth > 0) max_depth = g.depth;
        if (g.movetime_ms > 0) {
            budget_ms = g.movetime_ms;
        } else if (g.depth <= 0) {
            // No explicit limit at all — fall back to time
            // management derived from clock + increment.
            budget_ms = derive_movetime(g, s.board.side_to_move());
        }
    }

    s.tt.new_search();

    using clk = std::chrono::steady_clock;
    const auto start = clk::now();

    SearchResult last;
    bool have_result = false;

    for (int d = 1; d <= max_depth; ++d) {
        SearchLimits lim;
        lim.max_depth = d;
        // Match the analyzer / solve CLI optimization stack so
        // games played through UCI use the engine's strongest
        // configuration (LMR, history, aspiration, PVS, check
        // extensions; TT is already shared via `s.tt`).
        lim.enable_lmr        = true;
        lim.enable_history    = true;
        lim.enable_aspiration = true;
        lim.enable_pvs        = true;
        lim.enable_check_ext  = true;
        lim.enable_nmp_verify = true;
        lim.enable_futility   = true;
        // Past-game zobrists so the search detects 3-fold lines
        // reaching back into actually-played moves, plus a small
        // contempt so the engine prefers fighting on over a
        // forced draw when the static eval is positive.
        lim.position_history  = s.position_history;
        lim.contempt_cp       = 20;
        if (budget_ms > 0) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    clk::now() - start)
                    .count();
            const long long remaining = budget_ms - elapsed;
            if (remaining <= 0) break;
            lim.time_budget = std::chrono::milliseconds{remaining};
        }

        // Search runs on `BoardBitboard` for speed. Re-derive
        // from FEN each iteration so make/unmake inside the
        // search doesn't accumulate state across iterations.
        const std::string fen = serialize_fen(s.board);
        auto bb_work = BoardBitboard::from_fen(fen);
        SearchResult r;
        if (bb_work.has_value()) {
            r = Search::find_best(*bb_work, lim, &s.tt);
        } else {
            Board8x8Mailbox work = s.board;
            r = Search::find_best(work, lim, &s.tt);
        }

        if (r.completed_depth < d) {
            // The current iteration was aborted by the clock —
            // keep the previous completed result.
            if (!have_result) last = r; // mate at root edge case
            break;
        }

        last = r;
        have_result = true;
        emit_info(r, out);

        if (Search::is_mate_score(r.score)) break;
    }

    if (!have_result && last.best_move.from == Square::None) {
        // No legal moves (mate / stalemate at root). UCI spec
        // says "0000" is the null move.
        out << "bestmove 0000\n" << std::flush;
        return;
    }
    out << "bestmove " << to_uci(last.best_move) << '\n' << std::flush;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void print_uci_help(std::ostream& out) {
    out << "Usage: chesserazade uci\n"
        << "\n"
        << "Enter UCI (Universal Chess Interface) mode — line-based\n"
        << "stdin/stdout protocol used by Arena, CuteChess, SCID, ...\n"
        << "\n"
        << "Supported commands:\n"
        << "  uci, isready, ucinewgame, quit\n"
        << "  setoption name Hash value N   (MiB, "
        << MIN_HASH_MB << ".." << MAX_HASH_MB << ")\n"
        << "  setoption name Threads value N   (accepted, no-op)\n"
        << "  position {startpos | fen <fen>} [moves m1 m2 ...]\n"
        << "  go depth N | go movetime MS | go infinite\n"
        << "  go wtime X btime Y [winc Z] [binc W] [movestogo N]\n"
        << "\n"
        << "Not supported in 1.2: stop, ponder, multi-pv, searchmoves.\n";
}

} // namespace

UciSession::UciSession()
    : board(*Board8x8Mailbox::from_fen(STARTING_POSITION_FEN)),
      tt(entries_for_mb(DEFAULT_HASH_MB)),
      hash_mb(DEFAULT_HASH_MB) {}

bool process_uci_line(UciSession& s, const std::string& line,
                      std::ostream& out) {
    const auto toks = split_ws(line);
    if (toks.empty()) return false;
    const std::string& cmd = toks[0];

    if (cmd == "uci")              handle_uci(out);
    else if (cmd == "isready")     handle_isready(out);
    else if (cmd == "ucinewgame")  handle_ucinewgame(s);
    else if (cmd == "setoption")   handle_setoption(s, toks);
    else if (cmd == "position")    handle_position(s, toks);
    else if (cmd == "go")          handle_go(s, toks, out);
    else if (cmd == "quit")        return true;
    // `stop` and unknown commands are silently ignored per UCI
    // convention (the GUI may probe for extensions).
    return false;
}

int cmd_uci(std::span<const std::string_view> args) {
    for (const auto& a : args) {
        if (a == "-h" || a == "--help") {
            print_uci_help(std::cout);
            return 0;
        }
    }

    // UCI expects line buffering — make sure stdout flushes per
    // newline even when redirected to a pipe (the GUI side).
    std::cout.setf(std::ios::unitbuf);

    UciSession s;
    for (std::string line; std::getline(std::cin, line); ) {
        if (process_uci_line(s, line, std::cout)) break;
    }
    return 0;
}

} // namespace chesserazade::cli
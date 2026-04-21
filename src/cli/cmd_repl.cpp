#include "cli/cmd_repl.hpp"

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/game.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/move_generator.hpp>
#include <chesserazade/pgn.hpp>
#include <chesserazade/san.hpp>
#include <chesserazade/types.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chesserazade::cli {

// ---------------------------------------------------------------------------
// Option parsing
// ---------------------------------------------------------------------------

namespace {

struct ReplOptions {
    std::string fen = std::string{STARTING_POSITION_FEN};
    bool show_help = false;
};

struct ParseResult {
    ReplOptions options;
    std::string error;
};

ParseResult parse_repl_args(std::span<const std::string_view> args) {
    ParseResult r;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        if (a == "--help" || a == "-h") {
            r.options.show_help = true;
            return r;
        }
        if (a == "--fen") {
            if (i + 1 >= args.size()) {
                r.error = "--fen requires a value";
                return r;
            }
            r.options.fen = std::string{args[i + 1]};
            ++i;
            continue;
        }
        r.error = "unknown option '" + std::string{a} + "'";
        return r;
    }
    return r;
}

void print_repl_help(std::ostream& out) {
    out << "Usage: chesserazade repl [--fen <fen>]\n"
        << "       chesserazade play [--fen <fen>]    (alias)\n"
        << "\n"
        << "Start an interactive text session. `repl` and `play` are\n"
        << "aliases. There is no engine — moves are played by the\n"
        << "human on both sides.\n"
        << "\n"
        << "Options:\n"
        << "  --fen <fen>   Starting position (default: standard start).\n"
        << "  -h, --help    Show this message.\n"
        << "\n"
        << "Commands available inside the session (see `help` at the\n"
        << "prompt for the full list):\n"
        << "  <uci|san>     Play a move (e2e4, Nf3, O-O, e8=Q, ...).\n"
        << "  undo          Undo the last move.\n"
        << "  show          Render the current board.\n"
        << "  legal         List legal moves in UCI form.\n"
        << "  pgn [path]    Save the game as PGN (stdout if no path).\n"
        << "  load <path>   Load a PGN file and replay its moves.\n"
        << "  reset         Return to the starting position.\n"
        << "  quit | exit   Leave the session.\n";
}

void print_session_help(std::ostream& out) {
    out << "Commands:\n"
        << "  <uci|san>     Play a move. `e2e4`, `Nf3`, `O-O`, `e8=Q`.\n"
        << "                UCI and SAN are both accepted; UCI wins on\n"
        << "                a clash (`Nf3` can never be UCI).\n"
        << "  move <mv>     Same as above; `move` is optional.\n"
        << "  undo          Undo the last move.\n"
        << "  show          Render the current position.\n"
        << "  legal         Sorted UCI list of legal moves.\n"
        << "  pgn [path]    Save game as PGN; prints to stdout if\n"
        << "                no path is given.\n"
        << "  load <path>   Load a PGN file; its moves become the new\n"
        << "                game.\n"
        << "  reset         Discard history, return to starting pos.\n"
        << "  help          Print this message.\n"
        << "  quit | exit   Leave the session.\n";
}

// ---------------------------------------------------------------------------
// Board rendering — small local copy of cmd_show's ASCII path. When
// we have a shared render layer in 0.5+, both commands will use it.
// ---------------------------------------------------------------------------

void render_ascii(const Board& b, std::ostream& out) {
    for (int rank = 7; rank >= 0; --rank) {
        out << (rank + 1) << ' ';
        for (int file = 0; file < 8; ++file) {
            const auto sq = make_square(static_cast<File>(file),
                                        static_cast<Rank>(rank));
            const Piece p = b.piece_at(sq);
            out << ' ' << (p.is_none() ? '.' : piece_to_fen_char(p));
        }
        out << '\n';
    }
    out << "   a b c d e f g h\n";
    out << "side to move: "
        << (b.side_to_move() == Color::White ? "white" : "black") << '\n';
}

// ---------------------------------------------------------------------------
// Move input
// ---------------------------------------------------------------------------

/// True if `s` has the shape of a UCI move: 4 algebraic characters,
/// optionally followed by a promotion letter (q/r/b/n).
[[nodiscard]] bool looks_like_uci(std::string_view s) noexcept {
    if (s.size() != 4 && s.size() != 5) return false;
    auto is_file = [](char c) { return c >= 'a' && c <= 'h'; };
    auto is_rank = [](char c) { return c >= '1' && c <= '8'; };
    if (!is_file(s[0]) || !is_rank(s[1])) return false;
    if (!is_file(s[2]) || !is_rank(s[3])) return false;
    if (s.size() == 5) {
        const char p = s[4];
        if (p != 'q' && p != 'r' && p != 'b' && p != 'n') return false;
    }
    return true;
}

[[nodiscard]] std::optional<Move> resolve_uci(Board& board, std::string_view s) {
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
            default:  return std::nullopt;
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

/// Try UCI first (its grammar is unambiguous); fall back to SAN.
[[nodiscard]] std::optional<Move> resolve_move(Board& board,
                                               std::string_view s) {
    if (looks_like_uci(s)) {
        if (auto m = resolve_uci(board, s)) return m;
    }
    auto r = parse_san(board, s);
    if (r) return *r;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Session state + per-command handlers
// ---------------------------------------------------------------------------

struct Session {
    Game game;
    std::vector<std::pair<std::string, std::string>> tags = {
        {"Event",  "?"},
        {"Site",   "?"},
        {"Date",   "?"},
        {"Round",  "?"},
        {"White",  "?"},
        {"Black",  "?"},
        {"Result", "*"},
    };
    std::string termination = "*";
};

/// Split `line` on whitespace into tokens. A very small helper —
/// PGN-style quoted strings are not expected here.
[[nodiscard]] std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
        out.emplace_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

void do_show(Session& s, std::ostream& out) {
    render_ascii(s.game.current_position(), out);
}

void do_legal(Session& s, std::ostream& out) {
    auto& b = s.game.current_position();
    const MoveList ml = MoveGenerator::generate_legal(b);
    std::vector<std::string> uci;
    uci.reserve(ml.count);
    for (const Move& m : ml) uci.push_back(to_uci(m));
    std::sort(uci.begin(), uci.end());
    for (const auto& s1 : uci) out << s1 << '\n';
    out << "total: " << uci.size() << '\n';
}

/// Parse+play a move; print the written SAN on success, an error
/// otherwise. Returns true on success.
bool do_move(Session& s, std::string_view text, std::ostream& out,
             std::ostream& err) {
    auto& board = s.game.current_position();
    auto m = resolve_move(board, text);
    if (!m) {
        err << "not a legal move: " << text << '\n';
        return false;
    }
    const std::string san = to_san(board, *m);
    s.game.play_move(*m);
    out << san << '\n';
    return true;
}

void do_undo(Session& s, std::ostream& out, std::ostream& err) {
    if (s.game.undo_move()) {
        out << "undone (" << s.game.ply_count() << " ply left)\n";
    } else {
        err << "nothing to undo\n";
    }
}

void do_reset(Session& s, std::ostream& out) {
    s.game.reset_to_start();
    out << "reset\n";
}

void do_save_pgn(Session& s, const std::string& path, std::ostream& out,
                 std::ostream& err) {
    const std::string pgn = write_pgn(s.game, s.tags, s.termination);
    if (path.empty()) {
        out << pgn;
        return;
    }
    std::ofstream f(path);
    if (!f) {
        err << "could not open '" << path << "' for writing\n";
        return;
    }
    f << pgn;
    out << "wrote " << path << '\n';
}

void do_load_pgn(Session& s, const std::string& path, std::ostream& out,
                 std::ostream& err) {
    std::ifstream f(path);
    if (!f) {
        err << "could not open '" << path << "'\n";
        return;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    auto parsed = parse_pgn(buf.str());
    if (!parsed) {
        err << "parse error: " << parsed.error().message << '\n';
        return;
    }

    Board8x8Mailbox start;
    if (parsed->starting_fen) {
        auto r = Board8x8Mailbox::from_fen(*parsed->starting_fen);
        if (!r) {
            err << "PGN's FEN invalid: " << r.error().message << '\n';
            return;
        }
        start = *r;
    } else {
        auto r = Board8x8Mailbox::from_fen(std::string{STARTING_POSITION_FEN});
        start = *r;
    }
    s.game = Game(std::move(start));
    for (const Move& m : parsed->moves) {
        s.game.play_move(m);
    }
    s.tags = parsed->tags;
    s.termination = parsed->termination;
    out << "loaded " << parsed->moves.size() << " plies from " << path << '\n';
}

/// Dispatch one input line. Returns false only when the user asked
/// to quit.
bool dispatch_line(Session& s, const std::string& line,
                   std::ostream& out, std::ostream& err) {
    const auto toks = tokenize(line);
    if (toks.empty()) return true;

    const std::string& cmd = toks.front();

    if (cmd == "quit" || cmd == "exit") return false;
    if (cmd == "help" || cmd == "?")    { print_session_help(out); return true; }
    if (cmd == "show")                  { do_show(s, out);        return true; }
    if (cmd == "legal")                 { do_legal(s, out);       return true; }
    if (cmd == "undo")                  { do_undo(s, out, err);   return true; }
    if (cmd == "reset")                 { do_reset(s, out);       return true; }

    if (cmd == "pgn") {
        const std::string path = (toks.size() >= 2) ? toks[1] : std::string{};
        do_save_pgn(s, path, out, err);
        return true;
    }
    if (cmd == "load") {
        if (toks.size() < 2) { err << "load: path required\n"; return true; }
        do_load_pgn(s, toks[1], out, err);
        return true;
    }
    if (cmd == "move") {
        if (toks.size() < 2) { err << "move: notation required\n"; return true; }
        do_move(s, toks[1], out, err);
        return true;
    }

    // Fall-through: treat the first token as a move notation.
    do_move(s, cmd, out, err);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int cmd_repl(std::span<const std::string_view> args) {
    const auto parsed = parse_repl_args(args);
    if (parsed.options.show_help) {
        print_repl_help(std::cout);
        return 0;
    }
    if (!parsed.error.empty()) {
        std::cerr << "repl: " << parsed.error << "\n\n";
        print_repl_help(std::cerr);
        return 1;
    }

    auto start = Board8x8Mailbox::from_fen(parsed.options.fen);
    if (!start.has_value()) {
        std::cerr << "repl: " << start.error().message << '\n';
        return 1;
    }

    Session session{Game(std::move(*start)), {}, "*"};
    session.tags = {
        {"Event",  "?"}, {"Site", "?"}, {"Date", "?"}, {"Round", "?"},
        {"White",  "?"}, {"Black", "?"}, {"Result", "*"},
    };

    std::cout << "chesserazade repl — type `help` for commands, "
                 "`quit` to leave.\n";
    do_show(session, std::cout);

    std::string line;
    while (std::cout << "> ", std::getline(std::cin, line)) {
        if (!dispatch_line(session, line, std::cout, std::cerr)) {
            break;
        }
    }
    return 0;
}

} // namespace chesserazade::cli

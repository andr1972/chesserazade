#include "cli/cmd_show.hpp"

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/board.hpp>
#include <chesserazade/fen.hpp>
#include <chesserazade/types.hpp>

#include <iostream>
#include <string>
#include <string_view>

namespace chesserazade::cli {

namespace {

struct ShowOptions {
    std::string fen = std::string{STARTING_POSITION_FEN};
    bool unicode = false;
    bool show_help = false;
};

/// Argument parsing is intentionally a small hand-rolled routine. The
/// CLI surface grows slowly across versions (0.2 adds `moves`, 0.4
/// adds `play`/`repl`) and a bespoke parser keeps error messages
/// local and specific. No getopt, no Boost.program_options.
struct ParseResult {
    ShowOptions options;
    std::string error; // empty == OK
};

ParseResult parse_show_args(std::span<const std::string_view> args) {
    ParseResult r;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        if (arg == "--help" || arg == "-h") {
            r.options.show_help = true;
            return r;
        }
        if (arg == "--unicode") {
            r.options.unicode = true;
            continue;
        }
        if (arg == "--fen") {
            if (i + 1 >= args.size()) {
                r.error = "--fen requires a value";
                return r;
            }
            r.options.fen = std::string{args[i + 1]};
            ++i;
            continue;
        }
        r.error = "unknown option '" + std::string{arg} + "'";
        return r;
    }
    return r;
}

void print_show_help(std::ostream& out) {
    out << "Usage: chesserazade show [--fen <fen>] [--unicode]\n"
        << "\n"
        << "Render a chess position given by a FEN string.\n"
        << "\n"
        << "Options:\n"
        << "  --fen <fen>   Position to render. Defaults to the standard\n"
        << "                starting position. FEN input is ASCII only.\n"
        << "  --unicode     Use figurine glyphs (\xe2\x99\x94\xe2\x99\x99 ...) "
           "instead of ASCII letters.\n"
        << "                Display only; input is never parsed as Unicode.\n"
        << "  -h, --help    Show this message.\n";
}

/// Unicode figurine (UTF-8) for the given piece. Only called when the
/// user opts into Unicode output; FEN I/O never uses this.
std::string_view piece_to_unicode(Piece p) noexcept {
    if (p.is_none()) {
        return ".";
    }
    if (p.color == Color::White) {
        switch (p.type) {
            case PieceType::King:   return "\xe2\x99\x94"; // ♔
            case PieceType::Queen:  return "\xe2\x99\x95"; // ♕
            case PieceType::Rook:   return "\xe2\x99\x96"; // ♖
            case PieceType::Bishop: return "\xe2\x99\x97"; // ♗
            case PieceType::Knight: return "\xe2\x99\x98"; // ♘
            case PieceType::Pawn:   return "\xe2\x99\x99"; // ♙
            case PieceType::None:   return ".";
        }
    } else {
        switch (p.type) {
            case PieceType::King:   return "\xe2\x99\x9a"; // ♚
            case PieceType::Queen:  return "\xe2\x99\x9b"; // ♛
            case PieceType::Rook:   return "\xe2\x99\x9c"; // ♜
            case PieceType::Bishop: return "\xe2\x99\x9d"; // ♝
            case PieceType::Knight: return "\xe2\x99\x9e"; // ♞
            case PieceType::Pawn:   return "\xe2\x99\x9f"; // ♟
            case PieceType::None:   return ".";
        }
    }
    return ".";
}

std::string format_piece(Piece p, bool unicode) {
    if (unicode) {
        return std::string{piece_to_unicode(p)};
    }
    const char c = piece_to_fen_char(p);
    return std::string(1, c);
}

std::string format_castling(CastlingRights r) {
    if (!r.any()) {
        return "-";
    }
    std::string s;
    if (r.white_king_side)  s += 'K';
    if (r.white_queen_side) s += 'Q';
    if (r.black_king_side)  s += 'k';
    if (r.black_queen_side) s += 'q';
    return s;
}

void render_board(const Board& b, bool unicode, std::ostream& out) {
    for (int rank = 7; rank >= 0; --rank) {
        out << (rank + 1) << ' ';
        for (int file = 0; file < 8; ++file) {
            const auto sq = make_square(static_cast<File>(file),
                                        static_cast<Rank>(rank));
            out << ' ' << format_piece(b.piece_at(sq), unicode);
        }
        out << '\n';
    }
    out << "   a b c d e f g h\n";
    out << '\n';
    out << "side to move: "
        << (b.side_to_move() == Color::White ? "white" : "black") << '\n';
    out << "castling:     " << format_castling(b.castling_rights()) << '\n';
    out << "en passant:   "
        << (b.en_passant_square() == Square::None
                ? std::string{"-"}
                : to_algebraic(b.en_passant_square())) << '\n';
    out << "halfmove:     " << b.halfmove_clock() << '\n';
    out << "fullmove:     " << b.fullmove_number() << '\n';
}

} // namespace

int cmd_show(std::span<const std::string_view> args) {
    const auto parsed = parse_show_args(args);
    if (parsed.options.show_help) {
        print_show_help(std::cout);
        return 0;
    }
    if (!parsed.error.empty()) {
        std::cerr << "show: " << parsed.error << "\n\n";
        print_show_help(std::cerr);
        return 1;
    }

    const auto board = Board8x8Mailbox::from_fen(parsed.options.fen);
    if (!board.has_value()) {
        std::cerr << "show: " << board.error().message << '\n';
        return 1;
    }

    render_board(*board, parsed.options.unicode, std::cout);
    return 0;
}

} // namespace chesserazade::cli

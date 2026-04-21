#include "cli/cmd_moves.hpp"

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/move_generator.hpp>

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace chesserazade::cli {

namespace {

struct MovesOptions {
    std::string fen = std::string{STARTING_POSITION_FEN};
    bool show_help = false;
};

struct ParseResult {
    MovesOptions options;
    std::string error;
};

ParseResult parse_moves_args(std::span<const std::string_view> args) {
    ParseResult r;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        if (arg == "--help" || arg == "-h") {
            r.options.show_help = true;
            return r;
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

void print_moves_help(std::ostream& out) {
    out << "Usage: chesserazade moves [--fen <fen>]\n"
        << "\n"
        << "List all legal moves from a position, one UCI move per line.\n"
        << "\n"
        << "Options:\n"
        << "  --fen <fen>   Position to enumerate. Defaults to the standard\n"
        << "                starting position.\n"
        << "  -h, --help    Show this message.\n";
}

} // namespace

int cmd_moves(std::span<const std::string_view> args) {
    const auto parsed = parse_moves_args(args);
    if (parsed.options.show_help) {
        print_moves_help(std::cout);
        return 0;
    }
    if (!parsed.error.empty()) {
        std::cerr << "moves: " << parsed.error << "\n\n";
        print_moves_help(std::cerr);
        return 1;
    }

    auto board = Board8x8Mailbox::from_fen(parsed.options.fen);
    if (!board.has_value()) {
        std::cerr << "moves: " << board.error().message << '\n';
        return 1;
    }

    const MoveList ml = MoveGenerator::generate_legal(*board);

    // Sort for deterministic output so scripted diffing against a
    // reference engine stays stable regardless of generation order.
    std::vector<std::string> uci;
    uci.reserve(ml.count);
    for (const Move& m : ml) {
        uci.push_back(to_uci(m));
    }
    std::sort(uci.begin(), uci.end());

    for (const auto& s : uci) {
        std::cout << s << '\n';
    }
    std::cout << "total: " << uci.size() << '\n';
    return 0;
}

} // namespace chesserazade::cli

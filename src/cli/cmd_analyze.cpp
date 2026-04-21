#include "cli/cmd_analyze.hpp"

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/game.hpp>
#include <chesserazade/game_analyzer.hpp>
#include <chesserazade/pgn.hpp>
#include <chesserazade/transposition_table.hpp>

#include <charconv>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chesserazade::cli {

namespace {

struct AnalyzeOpts {
    std::string pgn_path;
    int depth = 10;
    bool show_help = false;
};

struct ParseResult {
    AnalyzeOpts opts;
    std::string error;
};

ParseResult parse_args(std::span<const std::string_view> args) {
    ParseResult r;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        if (a == "--help" || a == "-h") {
            r.opts.show_help = true;
            return r;
        }
        if (a == "--pgn") {
            if (i + 1 >= args.size()) {
                r.error = "--pgn requires a path";
                return r;
            }
            r.opts.pgn_path = std::string{args[i + 1]};
            ++i; continue;
        }
        if (a == "--depth") {
            if (i + 1 >= args.size()) {
                r.error = "--depth requires a value";
                return r;
            }
            const auto v = args[i + 1];
            int n = 0;
            const auto [_, ec] =
                std::from_chars(v.data(), v.data() + v.size(), n);
            if (ec != std::errc{} || n < 1) {
                r.error = "--depth must be a positive integer";
                return r;
            }
            r.opts.depth = n;
            ++i; continue;
        }
        r.error = "unknown option '" + std::string{a} + "'";
        return r;
    }
    if (r.opts.pgn_path.empty()) r.error = "--pgn <path> is required";
    return r;
}

void print_help(std::ostream& out) {
    out << "Usage: chesserazade analyze --pgn <path> [--depth N]\n"
        << "\n"
        << "Load a single-game PGN, analyze every ply at the given\n"
        << "fixed depth, and print an annotated PGN to stdout. Moves\n"
        << "losing > 300 cp vs the engine's best are tagged `??`;\n"
        << "150–300 gets `?`, 50–150 gets `?!`. Walking into or\n"
        << "missing a forced mate is always `??`.\n"
        << "\n"
        << "Options:\n"
        << "  --pgn <path>  PGN file with a single game (required).\n"
        << "  --depth N     Search depth in plies (default: 10).\n"
        << "  -h, --help    Show this message.\n";
}

} // namespace

int cmd_analyze(std::span<const std::string_view> args) {
    const auto parsed = parse_args(args);
    if (parsed.opts.show_help) {
        print_help(std::cout);
        return 0;
    }
    if (!parsed.error.empty()) {
        std::cerr << "analyze: " << parsed.error << "\n\n";
        print_help(std::cerr);
        return 1;
    }

    std::ifstream f(parsed.opts.pgn_path);
    if (!f) {
        std::cerr << "analyze: could not open '"
                  << parsed.opts.pgn_path << "'\n";
        return 1;
    }
    std::stringstream buf; buf << f.rdbuf();
    auto pgn = parse_pgn(buf.str());
    if (!pgn) {
        std::cerr << "analyze: PGN parse error: "
                  << pgn.error().message << '\n';
        return 1;
    }

    // Build a Game from the parsed PGN.
    Board8x8Mailbox start;
    if (pgn->starting_fen) {
        auto r = Board8x8Mailbox::from_fen(*pgn->starting_fen);
        if (!r) {
            std::cerr << "analyze: starting FEN invalid\n";
            return 1;
        }
        start = *r;
    } else {
        auto r = Board8x8Mailbox::from_fen(std::string{STARTING_POSITION_FEN});
        start = *r;
    }
    Game game(std::move(start));
    for (const Move& m : pgn->moves) game.play_move(m);

    TranspositionTable tt;
    AnalyzeOptions ao; ao.depth = parsed.opts.depth; ao.tt = &tt;
    const GameAnalysis ga = GameAnalyzer::analyze(game, ao);

    // Map analyzer verdicts onto per-move PGN annotations.
    std::vector<MoveAnnotation> anns;
    anns.reserve(ga.plies.size());
    for (const MoveAnalysis& p : ga.plies) {
        anns.push_back(MoveAnnotation{p.nag_suffix, p.comment});
    }

    std::cout << write_pgn(game, pgn->tags, pgn->termination, anns);
    return 0;
}

} // namespace chesserazade::cli

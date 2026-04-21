#include "cli/command_dispatch.hpp"

#include "cli/cmd_analyze.hpp"
#include "cli/cmd_fetch.hpp"
#include "cli/cmd_magics_gen.hpp"
#include "cli/cmd_moves.hpp"
#include "cli/cmd_perft.hpp"
#include "cli/cmd_repl.hpp"
#include "cli/cmd_show.hpp"
#include "cli/cmd_solve.hpp"
#include "cli/cmd_version.hpp"

#include <iostream>
#include <string_view>

namespace chesserazade::cli {

namespace {

void print_top_level_help(std::ostream& out) {
    out << "Usage: chesserazade <subcommand> [options]\n"
        << "\n"
        << "Subcommands:\n"
        << "  show      Render a position given by a FEN string.\n"
        << "  moves     List legal moves from a FEN position (UCI).\n"
        << "  perft     Count leaf nodes at a given depth from a position.\n"
        << "  repl      Interactive text session (play | PGN I/O).\n"
        << "  play      Alias for `repl`.\n"
        << "  solve     Search for the best move in a position.\n"
        << "  analyze   Annotate a PGN's moves with engine verdicts.\n"
        << "  fetch     Download a PGN / puzzle to the local cache.\n"
        << "  magics-gen  Regenerate the magic-bitboards constants file.\n"
        << "  version   Print the program version.\n"
        << "\n"
        << "Run `chesserazade <subcommand> --help` for subcommand-specific options.\n";
}

} // namespace

int dispatch(std::span<const std::string_view> args) {
    if (args.size() < 2) {
        print_top_level_help(std::cerr);
        return 1;
    }
    const std::string_view name = args[1];
    const auto rest = args.subspan(2);

    if (name == "show") {
        return cmd_show(rest);
    }
    if (name == "moves") {
        return cmd_moves(rest);
    }
    if (name == "perft") {
        return cmd_perft(rest);
    }
    if (name == "repl" || name == "play") {
        return cmd_repl(rest);
    }
    if (name == "solve") {
        return cmd_solve(rest);
    }
    if (name == "analyze") {
        return cmd_analyze(rest);
    }
    if (name == "fetch") {
        return cmd_fetch(rest);
    }
    if (name == "magics-gen") {
        return cmd_magics_gen(rest);
    }
    if (name == "version") {
        return cmd_version(rest);
    }
    if (name == "--help" || name == "-h" || name == "help") {
        print_top_level_help(std::cout);
        return 0;
    }

    std::cerr << "chesserazade: unknown subcommand '" << name << "'\n\n";
    print_top_level_help(std::cerr);
    return 1;
}

} // namespace chesserazade::cli

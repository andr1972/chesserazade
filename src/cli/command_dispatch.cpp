#include "cli/command_dispatch.hpp"

#include "cli/cmd_show.hpp"
#include "cli/cmd_version.hpp"

#include <iostream>
#include <string_view>

namespace chesserazade::cli {

namespace {

void print_top_level_help(std::ostream& out) {
    out << "Usage: chesserazade <subcommand> [options]\n"
        << "\n"
        << "Subcommands (0.1):\n"
        << "  show      Render a position given by a FEN string.\n"
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

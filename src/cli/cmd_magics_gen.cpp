#include "cli/cmd_magics_gen.hpp"

#include "board/magic.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace chesserazade::cli {

namespace {

void print_help(std::ostream& out) {
    out << "Usage: chesserazade magics-gen [--out <path>]\n"
        << "\n"
        << "Brute-force magic bitboards for rook and bishop on every\n"
        << "square and write the full table (square, magic, mask, shift)\n"
        << "to a file. Takes ~100 ms.\n"
        << "\n"
        << "Default output is `./magics.generated.txt` — never\n"
        << "`data/magics.txt` (the repo-shipped file), so a canonical\n"
        << "constants file cannot be clobbered by accident. To replace\n"
        << "the shipped file, move the generated one explicitly after\n"
        << "inspection.\n"
        << "\n"
        << "Options:\n"
        << "  --out <path>  Output file path (default:\n"
        << "                ./magics.generated.txt).\n"
        << "  -h, --help    Show this message.\n";
}

} // namespace

int cmd_magics_gen(std::span<const std::string_view> args) {
    std::string out_path = "magics.generated.txt";
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        if (a == "--help" || a == "-h") {
            print_help(std::cout);
            return 0;
        }
        if (a == "--out") {
            if (i + 1 >= args.size()) {
                std::cerr << "magics-gen: --out requires a value\n";
                print_help(std::cerr);
                return 1;
            }
            out_path = std::string{args[i + 1]};
            ++i;
            continue;
        }
        std::cerr << "magics-gen: unknown option '" << a << "'\n\n";
        print_help(std::cerr);
        return 1;
    }

    // Always generate from scratch here — ignore any file that
    // might have been loaded earlier in the process. Resetting
    // first makes that explicit.
    reset_magic_attacks();
    if (!init_magic_attacks()) {
        std::cerr << "magics-gen: failed to find magics (unexpected; "
                     "the search budget was exhausted).\n";
        return 1;
    }

    if (!write_magics_to_file(out_path)) {
        std::cerr << "magics-gen: could not write '" << out_path << "'\n";
        return 1;
    }
    std::cout << "magics-gen: wrote " << out_path << '\n';
    return 0;
}

} // namespace chesserazade::cli

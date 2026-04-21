#include "cli/cmd_show.hpp"

#include <iostream>

namespace chesserazade::cli {

int cmd_show(std::span<const std::string_view> /*args*/) {
    // Real implementation added in the next commit, after the Board and FEN
    // modules exist. Returning success here keeps the scaffold buildable.
    std::cout << "show: not yet implemented in this scaffold commit\n";
    return 0;
}

} // namespace chesserazade::cli

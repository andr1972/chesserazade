/// `chesserazade solve --fen <fen> --depth N` — search for the best
/// move in a position.
///
/// Front-end over `Search::find_best`. Prints the best move (SAN
/// + UCI), the score (centipawns or "mate in N"), the principal
/// variation as a SAN line, node count, and wall-clock elapsed.
#pragma once

#include <span>
#include <string_view>

namespace chesserazade::cli {

int cmd_solve(std::span<const std::string_view> args);

} // namespace chesserazade::cli

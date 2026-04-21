/// `chesserazade analyze --pgn <file> --depth N` — annotate every
/// move of a PGN with the engine's verdict.
///
/// Loads the file, runs `GameAnalyzer::analyze`, and writes an
/// annotated PGN (original tags + STR + NAG glyphs on suspect
/// moves + `{best was …}` comments) to stdout.
#pragma once

#include <span>
#include <string_view>

namespace chesserazade::cli {

int cmd_analyze(std::span<const std::string_view> args);

} // namespace chesserazade::cli

/// `chesserazade fetch` — pull PGNs / puzzles from the internet
/// into a local cache.
///
/// Two modes:
///   * `--url <URL>` — non-interactive, scripted use. The URL
///     is fetched (via `curl`) and the response body is written
///     to `~/.cache/chesserazade/<hash>.pgn` (or `--out <path>`
///     if supplied). The cache path is printed on stdout.
///   * No flags — interactive menu: enter a URL, or pick a
///     preset (Lichess puzzle by ID). Before any network call
///     the CLI prints the URL it is about to hit and asks for
///     confirmation.
///
/// The fetched file is intended to be fed into `analyze --pgn`
/// or `repl`'s `load` command.
#pragma once

#include <span>
#include <string_view>

namespace chesserazade::cli {

int cmd_fetch(std::span<const std::string_view> args);

} // namespace chesserazade::cli

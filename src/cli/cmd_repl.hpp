// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `chesserazade repl` (alias: `play`) — interactive text session.
///
/// A minimal REPL for manually driving a `Game`: enter moves (UCI
/// or SAN), undo, show, save/load PGN, list legal moves, quit.
/// There is no engine behind it — this is a two-humans-at-a-terminal
/// or a trace-by-hand tool. The search layer arrives in 0.5 and will
/// plug in alongside the commands here.
#pragma once

#include <span>
#include <string_view>

namespace chesserazade::cli {

int cmd_repl(std::span<const std::string_view> args);

} // namespace chesserazade::cli

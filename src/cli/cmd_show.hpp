// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `chesserazade show --fen <fen>` — render a position to the terminal.
///
/// By default the renderer uses ASCII letters for pieces (uppercase for
/// white, lowercase for black). Pass `--unicode` to use figurine glyphs
/// (`♔♕♖♗♘♙…`). Unicode is *output only* — FEN input is always ASCII.
#pragma once

#include <span>
#include <string_view>

namespace chesserazade::cli {

int cmd_show(std::span<const std::string_view> args);

} // namespace chesserazade::cli

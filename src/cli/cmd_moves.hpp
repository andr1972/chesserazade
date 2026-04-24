// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `chesserazade moves --fen <fen>` — list the legal moves from a position.
///
/// Output format: one UCI-encoded move per line, sorted by from-square then
/// to-square. The count is printed on the final line. Intended for manual
/// inspection (e.g. "does the generator emit the moves I expect here?")
/// and for scripted diffing against a reference engine.
#pragma once

#include <span>
#include <string_view>

namespace chesserazade::cli {

int cmd_moves(std::span<const std::string_view> args);

} // namespace chesserazade::cli

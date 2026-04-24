// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `chesserazade perft --depth N [--fen <fen>] [--divide]` — enumerate
/// leaf nodes of the move tree.
///
/// Perft (performance test, move path enumeration) is the standard
/// correctness probe for a chess move generator. With `--divide`, prints
/// one line per root move with its sub-tree count; without, prints only
/// the total. See https://www.chessprogramming.org/Perft for background.
#pragma once

#include <span>
#include <string_view>

namespace chesserazade::cli {

int cmd_perft(std::span<const std::string_view> args);

} // namespace chesserazade::cli

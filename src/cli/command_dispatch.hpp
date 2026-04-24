// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Command dispatch for the `chesserazade` CLI.
///
/// Each subcommand lives in its own `cmd_*.cpp` file and exposes a single
/// entry point of the shape `int run(std::span<const std::string_view>)`.
/// `dispatch()` looks up the subcommand by name and calls it, returning
/// the exit code unchanged.
#pragma once

#include <span>
#include <string_view>

namespace chesserazade::cli {

/// Run the subcommand named in `args[1]` with the remaining arguments.
///
/// `args[0]` is the program name (argv[0]) and is ignored. If the name is
/// missing or unknown, a short help message is printed and a non-zero
/// exit code is returned.
int dispatch(std::span<const std::string_view> args);

} // namespace chesserazade::cli

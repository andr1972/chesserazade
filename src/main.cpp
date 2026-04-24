// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Chesserazade entry point.
///
/// The executable is a thin shell that parses argv[1] as a subcommand name
/// and forwards the remaining arguments to the matching handler. All
/// engine logic lives in `chesserazade_core`; this file intentionally
/// contains nothing but dispatch glue so that a reader can see the top-level
/// command surface in one place.
#include "board/magic.hpp"
#include "cli/command_dispatch.hpp"

#include <span>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    // Pick the fastest slider-attack implementation this build
    // + CPU supports. Returns "pext" / "magic-file" /
    // "magic-gen" / "loop"; any fallback happens silently —
    // the engine works identically at every level, just faster.
    (void)chesserazade::init_slider_attacks();

    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return chesserazade::cli::dispatch(std::span<const std::string_view>{args});
}

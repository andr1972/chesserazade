// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `chesserazade magics-gen [--out <path>]` — search for and
/// write a magic-bitboards constants file.
///
/// Brute-force finds a magic for every rook- and bishop-
/// square, then writes the full table (square, magic, mask,
/// shift) to a file the engine can load via the standard
/// lookup chain.
///
/// Default output path is `magics.generated.txt` in the
/// current working directory — never `data/magics.txt` —
/// so a repo-shipped constants file is never overwritten by
/// accident. The user can then inspect / compare / rename
/// the generated file manually.
#pragma once

#include <span>
#include <string_view>

namespace chesserazade::cli {

int cmd_magics_gen(std::span<const std::string_view> args);

} // namespace chesserazade::cli

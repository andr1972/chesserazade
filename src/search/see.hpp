// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Static Exchange Evaluation — given a capture, simulates the full
/// piece-exchange sequence on the destination square and returns the
/// net material balance (centipawns, signed, from the mover's view).
/// Both sides choose at each step whether to continue capturing or
/// stop, assuming optimal play.
///
/// Used by move ordering to demote losing captures (SEE < 0) below
/// killers and counter-moves so legitimate quiet plans get tried
/// first. See https://www.chessprogramming.org/Static_Exchange_Evaluation.
#pragma once

#include <chesserazade/move.hpp>

namespace chesserazade {

class BoardBitboard;

/// Returns the SEE result for the capture `m` on `b`. Pre-conditions:
///   - `m` is a capture / en-passant / promotion-capture (caller checks).
///   - `b` is the position before the move is made.
[[nodiscard]] int see(const BoardBitboard& b, const Move& m) noexcept;

} // namespace chesserazade

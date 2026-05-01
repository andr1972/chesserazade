// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `chesserazade uci` — UCI (Universal Chess Interface) mode.
///
/// Line-based stdin/stdout protocol used by Arena / CuteChess /
/// SCID to drive an engine. The subcommand blocks reading stdin
/// and replies on stdout until it receives `quit` or EOF.
///
/// Reference: https://wbec-ridderkerk.nl/html/UCIProtocol.html
#pragma once

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/board.hpp>
#include <chesserazade/search.hpp>
#include <chesserazade/transposition_table.hpp>

#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chesserazade::cli {

/// Mutable state carried across UCI lines: the current position,
/// the transposition table (shared across `go` calls), and the
/// currently-configured Hash size in MiB. Exposed in the header
/// so the test suite can drive the UCI state machine a line at
/// a time without spawning a subprocess.
struct UciSession {
    Board8x8Mailbox board;
    TranspositionTable tt;
    int hash_mb;
    /// LMR reduction policy for this session. Default is the
    /// long-standing R=1 behaviour; GUIs / match.py can override
    /// via 'setoption name LmrMode value <X>'.
    SearchLimits::LmrMode lmr_mode = SearchLimits::LmrMode::Constant1;
    /// PVS scout window and LMP cut, controllable via UCI options
    /// EnablePvs / EnableLmp. PVS defaults on, LMP off (chesserazade's
    /// quiet ordering doesn't yet make LMP a net win).
    bool enable_pvs = true;
    bool enable_lmp = false;
    /// Zobrist keys of every position reached *before* the
    /// current `board` — i.e. start position + after each move
    /// played, with the very last position (= `board` itself)
    /// excluded. Passed to `SearchLimits::position_history` so
    /// the search detects a 3-fold-style repetition reaching
    /// back into the played game.
    std::vector<ZobristKey> position_history;

    UciSession();
};

/// Process one UCI line. Returns `true` if the engine should
/// exit its read loop (i.e. `quit` or EOF-equivalent). All
/// engine→GUI output goes to `out`.
bool process_uci_line(UciSession& s,
                      const std::string& line,
                      std::ostream& out);

int cmd_uci(std::span<const std::string_view> args);

} // namespace chesserazade::cli
// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include "cli/cmd_solve.hpp"

#include "board/board8x8_mailbox.hpp"
#include "board/board_bitboard.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/puzzle_solver.hpp>
#include <chesserazade/san.hpp>
#include <chesserazade/search.hpp>
#include <chesserazade/transposition_table.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace chesserazade::cli {

namespace {

struct SolveOptions {
    std::string fen = std::string{STARTING_POSITION_FEN};
    int depth = -1;
    int time_ms = 0;
    std::uint64_t nodes = 0;
    int mate_in = 0; // 0 = normal search; N>0 = puzzle solver
    bool show_help = false;
};

struct ParseResult {
    SolveOptions options;
    std::string error;
};

ParseResult parse_solve_args(std::span<const std::string_view> args) {
    ParseResult r;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        if (a == "--help" || a == "-h") {
            r.options.show_help = true;
            return r;
        }
        if (a == "--fen") {
            if (i + 1 >= args.size()) {
                r.error = "--fen requires a value";
                return r;
            }
            r.options.fen = std::string{args[i + 1]};
            ++i;
            continue;
        }
        if (a == "--depth" || a == "--time-ms" || a == "--nodes"
            || a == "--mate-in") {
            if (i + 1 >= args.size()) {
                r.error = std::string{a} + " requires a value";
                return r;
            }
            const auto v = args[i + 1];
            long long n = 0;
            const auto [_, ec] =
                std::from_chars(v.data(), v.data() + v.size(), n);
            if (ec != std::errc{} || n < 0) {
                r.error = std::string{a} + " must be a non-negative integer";
                return r;
            }
            if (a == "--depth")         r.options.depth   = static_cast<int>(n);
            else if (a == "--time-ms")  r.options.time_ms = static_cast<int>(n);
            else if (a == "--mate-in")  r.options.mate_in = static_cast<int>(n);
            else                        r.options.nodes   =
                static_cast<std::uint64_t>(n);
            ++i;
            continue;
        }
        r.error = "unknown option '" + std::string{a} + "'";
        return r;
    }
    // At least one limit must be set.
    if (r.options.depth < 0 && r.options.time_ms == 0
        && r.options.nodes == 0 && r.options.mate_in == 0) {
        r.error = "--depth, --time-ms, --nodes, or --mate-in is required";
    }
    return r;
}

void print_solve_help(std::ostream& out) {
    out << "Usage: chesserazade solve (--depth N | --time-ms T | --nodes N\n"
        << "                           | --mate-in N) [--fen <fen>]\n"
        << "\n"
        << "Search for the best move using alpha-beta negamax with\n"
        << "iterative deepening, move ordering, TT, and quiescence.\n"
        << "At least one budget must be set; whichever fires first\n"
        << "stops the search.\n"
        << "\n"
        << "Options:\n"
        << "  --depth N     Cap on plies (iterative deepening runs 1..N).\n"
        << "  --time-ms T   Wall-clock budget in milliseconds.\n"
        << "  --nodes N     Visited-node budget.\n"
        << "  --mate-in N   Puzzle mode: search to a depth that will see\n"
        << "                a forced mate in N full chess moves. Prints\n"
        << "                'mate in N' on success, 'no mate found' on\n"
        << "                failure (within the search depth).\n"
        << "  --fen <fen>   Starting position (default: standard start).\n"
        << "  -h, --help    Show this message.\n";
}

/// Format the score as either "+123 cp" / "-45 cp" (centipawns) or
/// "mate in N" / "mated in N" (moves, not plies — the human-visible
/// form converts plies to full chess moves, rounding up for white's
/// half-move).
[[nodiscard]] std::string format_score(int score) {
    if (!Search::is_mate_score(score)) {
        std::string s;
        if (score >= 0) s.push_back('+');
        s += std::to_string(score);
        s += " cp";
        return s;
    }
    const int plies = Search::plies_to_mate(score);
    const int abs_plies = plies > 0 ? plies : -plies;
    const int moves = (abs_plies + 1) / 2;
    return (plies > 0 ? "mate in " : "mated in ") + std::to_string(moves);
}

/// Render the PV as a SAN line: "1. e4 e5 2. Nf3 Nc6 ..." The
/// numbering starts at the board's fullmove counter.
[[nodiscard]] std::string format_pv(Board8x8Mailbox board,
                                    const std::vector<Move>& pv) {
    std::ostringstream out;
    int move_number = board.fullmove_number();
    bool white_to_move = board.side_to_move() == Color::White;

    for (std::size_t i = 0; i < pv.size(); ++i) {
        if (white_to_move) {
            out << move_number << ". ";
        } else if (i == 0) {
            out << move_number << "... ";
        }
        out << to_san(board, pv[i]);
        if (i + 1 < pv.size()) out << ' ';
        board.make_move(pv[i]);
        if (!white_to_move) ++move_number;
        white_to_move = !white_to_move;
    }
    return out.str();
}

} // namespace

int cmd_solve(std::span<const std::string_view> args) {
    const auto parsed = parse_solve_args(args);
    if (parsed.options.show_help) {
        print_solve_help(std::cout);
        return 0;
    }
    if (!parsed.error.empty()) {
        std::cerr << "solve: " << parsed.error << "\n\n";
        print_solve_help(std::cerr);
        return 1;
    }

    auto board = Board8x8Mailbox::from_fen(parsed.options.fen);
    if (!board.has_value()) {
        std::cerr << "solve: " << board.error().message << '\n';
        return 1;
    }

    // Preserve a copy of the starting board for PV rendering (we
    // need SAN-against-pre-move-positions which requires a fresh
    // walk from the start — the `Search` mutates its board but
    // restores it, so technically we could reuse, but a copy keeps
    // the intent local and cheap).
    const Board8x8Mailbox starting = *board;

    SearchLimits limits;
    limits.max_depth = parsed.options.depth > 0 ? parsed.options.depth
                                                : Search::MAX_DEPTH;
    limits.time_budget = std::chrono::milliseconds{parsed.options.time_ms};
    limits.node_budget = parsed.options.nodes;
    // Match the analyzer's default solve stack: bitboard board,
    // TT, LMR, history, aspiration, PVS, check extensions. Without
    // these the CLI's reachable depth lags the analyzer's at the
    // same time budget by several plies.
    limits.enable_lmr        = true;
    limits.enable_history    = true;
    limits.enable_aspiration = true;
    limits.enable_pvs        = true;
    limits.enable_check_ext  = true;

    // Allocate a default 1M-entry TT (~16 MiB). A future --hash
    // flag will expose the size when that matters.
    TranspositionTable tt;

    // Search runs on `BoardBitboard` for speed; the mailbox copy
    // above is preserved for SAN-based PV rendering.
    auto bb = BoardBitboard::from_fen(parsed.options.fen);
    if (!bb.has_value()) {
        std::cerr << "solve: " << bb.error().message << '\n';
        return 1;
    }

    SearchResult r;
    if (parsed.options.mate_in > 0) {
        r = PuzzleSolver::solve_mate_in(*bb, parsed.options.mate_in, &tt);
    } else {
        // Drive iterative deepening externally (one depth per
        // call) so we can print intermediate iterations the way
        // the analyzer's panel does. The TT is shared across
        // calls so re-running 1..d each iteration is cheap.
        using clk = std::chrono::steady_clock;
        const auto begin = clk::now();
        bool have_result = false;
        std::uint64_t total_nodes = 0;
        long long total_ms = 0;
        // `progress_nodes` reports the live node count from inside
        // `find_best` and — unlike `SearchResult.nodes` — is *not*
        // rolled back when an iteration aborts. We read it after
        // each call to count the work the aborted iteration
        // actually did, which would otherwise be lost.
        std::atomic<std::uint64_t> progress{0};
        for (int d = 1; d <= limits.max_depth; ++d) {
            SearchLimits lim = limits;
            lim.max_depth = d;
            lim.progress_nodes = &progress;
            if (parsed.options.time_ms > 0) {
                const long long remaining =
                    static_cast<long long>(parsed.options.time_ms) - total_ms;
                if (remaining <= 0) break;
                lim.time_budget = std::chrono::milliseconds{remaining};
            }
            const auto iter_begin = clk::now();
            // Reset to 0 before the call — `find_best` stores
            // (not adds) into `progress_nodes`, so without the
            // reset the value would dip then climb back up,
            // making a delta meaningless.
            progress.store(0, std::memory_order_relaxed);
            const SearchResult ir = Search::find_best(*bb, lim, &tt);
            const long long iter_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    clk::now() - iter_begin).count();
            // Prefer the larger of the two — `ir.nodes` is exact
            // for completed iterations but rolled back on abort;
            // `progress` is approximate (stale by one budget-check
            // tick) but never rolls back. The max captures both.
            const std::uint64_t actual_nodes = std::max<std::uint64_t>(
                ir.nodes, progress.load(std::memory_order_relaxed));
            if (ir.completed_depth < d) {
                if (!have_result) r = ir;
                total_nodes += actual_nodes;
                total_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        clk::now() - begin).count();
                break;
            }
            r = ir;
            have_result = true;
            total_nodes += actual_nodes;
            total_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    clk::now() - begin).count();
            const double mnps = iter_ms > 0
                ? static_cast<double>(ir.nodes) / 1000.0
                  / static_cast<double>(iter_ms)
                : 0.0;

            std::cout << "depth  " << ir.completed_depth << "  score ";
            if (Search::is_mate_score(ir.score)) {
                const int plies = Search::plies_to_mate(ir.score);
                const int abs_plies = plies > 0 ? plies : -plies;
                const int moves = (abs_plies + 1) / 2;
                std::cout << "mate " << (plies > 0 ? moves : -moves);
            } else {
                std::cout << "cp " << ir.score;
            }
            std::cout << " nodes " << ir.nodes
                      << "  time " << iter_ms << " ms";
            std::printf("  speed %.2f Mn/s\n", mnps);
            std::cout << "  pv";
            for (const Move& m : ir.principal_variation) {
                std::cout << ' ' << to_uci(m);
            }
            std::cout << '\n';
            std::cout.flush();

            if (Search::is_mate_score(ir.score)) break;
        }
        // Overwrite the final result's nodes/elapsed with the
        // cumulative totals so the summary block reports the
        // whole search rather than just the last ID call.
        r.nodes = total_nodes;
        r.elapsed = std::chrono::milliseconds{total_ms};
        std::cout << '\n';
    }

    if (parsed.options.mate_in > 0 && !Search::is_mate_score(r.score)) {
        std::cout << "no mate in " << parsed.options.mate_in
                  << " found (best score " << format_score(r.score)
                  << " at depth " << r.completed_depth << ")\n";
        return 1;
    }

    std::cout << "best:  "
              << to_san(*board, r.best_move) << "  ("
              << to_uci(r.best_move) << ")\n";
    std::cout << "score: " << format_score(r.score) << '\n';
    std::cout << "depth: " << r.completed_depth << '\n';
    if (!r.principal_variation.empty()) {
        std::cout << "pv:    " << format_pv(starting, r.principal_variation)
                  << '\n';
    }
    std::cout << "nodes: " << r.nodes << '\n';
    if (r.tt_probes > 0) {
        const double hit_rate =
            100.0 * static_cast<double>(r.tt_hits)
                  / static_cast<double>(r.tt_probes);
        std::printf("tt:    %llu probes, %llu hits (%.1f%%)\n",
                    static_cast<unsigned long long>(r.tt_probes),
                    static_cast<unsigned long long>(r.tt_hits),
                    hit_rate);
    }
    std::cout << "time:  " << r.elapsed.count() << " ms\n";
    if (r.elapsed.count() > 0) {
        const double mnps = static_cast<double>(r.nodes) / 1000.0
                          / static_cast<double>(r.elapsed.count());
        std::printf("speed: %.2f Mn/s\n", mnps);
    }
    return 0;
}

} // namespace chesserazade::cli

// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include "cli/cmd_solve.hpp"

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/puzzle_solver.hpp>
#include <chesserazade/san.hpp>
#include <chesserazade/search.hpp>
#include <chesserazade/transposition_table.hpp>

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

    // Allocate a default 1M-entry TT (~16 MiB). A future --hash
    // flag will expose the size when that matters.
    TranspositionTable tt;

    SearchResult r;
    if (parsed.options.mate_in > 0) {
        r = PuzzleSolver::solve_mate_in(*board, parsed.options.mate_in, &tt);
    } else {
        r = Search::find_best(*board, limits, &tt);
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
    return 0;
}

} // namespace chesserazade::cli

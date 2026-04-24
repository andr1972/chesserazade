// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include "cli/cmd_perft.hpp"

#include "board/board8x8_mailbox.hpp"
#include "board/board_bitboard.hpp"

#include <chesserazade/fen.hpp>
#include <chesserazade/move.hpp>
#include <chesserazade/move_generator.hpp>

#include <memory>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chesserazade::cli {

namespace {

enum class BoardKind { Mailbox, Bitboard };

struct PerftOptions {
    std::string fen = std::string{STARTING_POSITION_FEN};
    int depth = -1;
    bool divide = false;
    BoardKind board_kind = BoardKind::Mailbox;
    bool show_help = false;
};

struct ParseResult {
    PerftOptions options;
    std::string error;
};

ParseResult parse_perft_args(std::span<const std::string_view> args) {
    ParseResult r;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        if (arg == "--help" || arg == "-h") {
            r.options.show_help = true;
            return r;
        }
        if (arg == "--divide") {
            r.options.divide = true;
            continue;
        }
        if (arg == "--fen") {
            if (i + 1 >= args.size()) {
                r.error = "--fen requires a value";
                return r;
            }
            r.options.fen = std::string{args[i + 1]};
            ++i;
            continue;
        }
        if (arg == "--board") {
            if (i + 1 >= args.size()) {
                r.error = "--board requires a value";
                return r;
            }
            const auto v = args[i + 1];
            if (v == "mailbox")       r.options.board_kind = BoardKind::Mailbox;
            else if (v == "bitboard") r.options.board_kind = BoardKind::Bitboard;
            else {
                r.error = "--board must be 'mailbox' or 'bitboard'";
                return r;
            }
            ++i;
            continue;
        }
        if (arg == "--depth") {
            if (i + 1 >= args.size()) {
                r.error = "--depth requires a value";
                return r;
            }
            const auto v = args[i + 1];
            int n = 0;
            const auto* first = v.data();
            const auto* last = v.data() + v.size();
            const auto [_, ec] = std::from_chars(first, last, n);
            if (ec != std::errc{} || n < 0) {
                r.error = "--depth must be a non-negative integer";
                return r;
            }
            r.options.depth = n;
            ++i;
            continue;
        }
        r.error = "unknown option '" + std::string{arg} + "'";
        return r;
    }
    if (r.options.depth < 0) {
        r.error = "--depth is required";
    }
    return r;
}

void print_perft_help(std::ostream& out) {
    out << "Usage: chesserazade perft --depth N [--fen <fen>] [--divide]\n"
        << "                          [--board mailbox|bitboard]\n"
        << "\n"
        << "Count leaf nodes at depth N from the given position.\n"
        << "\n"
        << "Options:\n"
        << "  --depth N     Search depth (required, >= 0).\n"
        << "  --fen <fen>   Position to search from. Defaults to the\n"
        << "                standard starting position.\n"
        << "  --divide      Print per-root-move counts, not just the total.\n"
        << "  --board K     'mailbox' (classical array, default) or\n"
        << "                'bitboard' (64-bit piece sets — faster).\n"
        << "  -h, --help    Show this message.\n";
}

std::uint64_t perft(Board& b, int depth) {
    if (depth == 0) {
        return 1;
    }
    const MoveList ml = MoveGenerator::generate_legal(b);
    if (depth == 1) {
        return static_cast<std::uint64_t>(ml.count);
    }
    std::uint64_t nodes = 0;
    for (const Move& m : ml) {
        b.make_move(m);
        nodes += perft(b, depth - 1);
        b.unmake_move(m);
    }
    return nodes;
}

} // namespace

int cmd_perft(std::span<const std::string_view> args) {
    const auto parsed = parse_perft_args(args);
    if (parsed.options.show_help) {
        print_perft_help(std::cout);
        return 0;
    }
    if (!parsed.error.empty()) {
        std::cerr << "perft: " << parsed.error << "\n\n";
        print_perft_help(std::cerr);
        return 1;
    }

    // Build the requested concrete Board and hold it through a
    // Board* for the perft helpers — both implementations share
    // the same interface.
    std::unique_ptr<Board> board;
    if (parsed.options.board_kind == BoardKind::Bitboard) {
        auto r = BoardBitboard::from_fen(parsed.options.fen);
        if (!r.has_value()) {
            std::cerr << "perft: " << r.error().message << '\n';
            return 1;
        }
        board = std::make_unique<BoardBitboard>(std::move(*r));
    } else {
        auto r = Board8x8Mailbox::from_fen(parsed.options.fen);
        if (!r.has_value()) {
            std::cerr << "perft: " << r.error().message << '\n';
            return 1;
        }
        board = std::make_unique<Board8x8Mailbox>(std::move(*r));
    }

    const int depth = parsed.options.depth;

    if (!parsed.options.divide) {
        const std::uint64_t n = perft(*board, depth);
        std::cout << "nodes: " << n << '\n';
        return 0;
    }

    // Divide mode: print a sorted list of "uci: count" lines so the
    // output can be diffed against Stockfish's `go perft` output.
    const MoveList ml = MoveGenerator::generate_legal(*board);
    std::vector<std::pair<std::string, std::uint64_t>> rows;
    rows.reserve(ml.count);
    std::uint64_t total = 0;
    for (const Move& m : ml) {
        board->make_move(m);
        const std::uint64_t sub = (depth <= 1) ? 1ULL : perft(*board, depth - 1);
        board->unmake_move(m);
        rows.emplace_back(to_uci(m), sub);
        total += sub;
    }
    // std::pair compares lexicographically; first element (UCI string)
    // determines the order, which is what we want here.
    std::sort(rows.begin(), rows.end());
    for (const auto& row : rows) {
        std::cout << row.first << ": " << row.second << '\n';
    }
    std::cout << "\nmoves: " << rows.size() << '\n';
    std::cout << "nodes: " << total << '\n';
    return 0;
}

} // namespace chesserazade::cli

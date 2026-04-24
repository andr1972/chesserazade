// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Perft benchmark — not a correctness test.
///
/// Runs the six standard positions from the Chess Programming Wiki
/// (https://www.chessprogramming.org/Perft_Results) up to a configurable
/// depth and prints nodes, wall-clock time, and Mnodes/sec per position.
///
/// This binary is intentionally separated from the Catch2 test suite.
/// Tests verify *what* we compute; the benchmark measures *how fast*,
/// and so it must NOT fail the build when it is slow — it only reports.
///
/// Usage:
///   perft_bench                    # default: all positions, depth 6
///                                  # (initial) / depth 5 (others)
///   perft_bench --depth 5          # cap the depth everywhere
///   perft_bench --only initial     # run one position only
///   perft_bench --help
///
/// Reading the numbers: a mailbox engine with a naive legality filter
/// (make / is_in_check / unmake per pseudo-legal move) is expected to
/// land in the low-millions of nodes per second in release mode.
/// A bitboard rewrite (planned for 1.1) will beat these numbers by
/// one to two orders of magnitude — that is the point of keeping the
/// benchmark around: it becomes the baseline to measure against.
#include "board/board8x8_mailbox.hpp"
#include "board/board_bitboard.hpp"
#include "board/magic.hpp"

#include <chesserazade/move_generator.hpp>
#include <chesserazade/bitboard.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

using namespace chesserazade;
using clk = std::chrono::steady_clock;

namespace {

struct BenchPosition {
    std::string_view name;
    std::string_view fen;
    /// Default depth for this position. Position 2 (Kiwipete) and
    /// positions 4–6 blow up rapidly at depth 6, so they default to 5.
    int default_depth;
};

/// The six standard positions, with depth defaults chosen so that a
/// full run finishes in a couple of minutes on a mailbox engine.
constexpr BenchPosition POSITIONS[] = {
    {"initial",
     "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
     6},
    {"kiwipete",
     "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
     5},
    {"pos3",
     "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
     6},
    {"pos4",
     "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
     5},
    {"pos5",
     "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
     5},
    {"pos6",
     "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
     5},
};

std::uint64_t perft(Board& b, int depth) {
    if (depth == 0) return 1;
    const MoveList ml = MoveGenerator::generate_legal(b);
    if (depth == 1) return static_cast<std::uint64_t>(ml.count);
    std::uint64_t n = 0;
    for (const Move& m : ml) {
        b.make_move(m);
        n += perft(b, depth - 1);
        b.unmake_move(m);
    }
    return n;
}

void print_help() {
    std::puts(
        "Usage: perft_bench [options]\n"
        "\n"
        "Run perft on the six standard positions and print timings.\n"
        "Each position is run twice — once against the mailbox board\n"
        "and once against the bitboard board — and the speedup is\n"
        "shown side-by-side. Node counts must agree; a mismatch is\n"
        "flagged on stderr.\n"
        "\n"
        "Options:\n"
        "  --depth N     Cap the depth on all positions at N (default: each\n"
        "                position's built-in default — 6 or 5).\n"
        "  --only NAME   Run only the named position\n"
        "                (initial | kiwipete | pos3 | pos4 | pos5 | pos6).\n"
        "  -h, --help    Show this message.\n");
}

/// Time a single perft run on `b` at `depth`. Returns seconds.
double time_perft(Board& b, int depth, std::uint64_t& out_nodes) {
    const auto start = clk::now();
    out_nodes = perft(b, depth);
    const auto end = clk::now();
    return std::chrono::duration<double>(end - start).count();
}

[[nodiscard]] double mnps_of(std::uint64_t n, double s) noexcept {
    return s > 0.0 ? static_cast<double>(n) / s / 1.0e6 : 0.0;
}

/// Run perft on `fen` at `depth` against a fresh
/// `BoardBitboard` and return (nodes, seconds). The current
/// global slider-attack implementation is whatever the caller
/// switched to — we do not re-init here.
void time_bb(std::string_view fen, int depth,
             std::uint64_t& out_nodes, double& out_secs) {
    auto r = BoardBitboard::from_fen(fen);
    BoardBitboard b = *r;
    out_secs = time_perft(b, depth, out_nodes);
}

void run_one(const BenchPosition& p, int depth) {
    auto mb_r = Board8x8Mailbox::from_fen(p.fen);
    if (!mb_r) {
        std::fprintf(stderr, "%s: bad FEN\n", p.name.data());
        return;
    }
    Board8x8Mailbox b_mb = *mb_r;

    std::uint64_t nodes_mb = 0;
    const double t_mb = time_perft(b_mb, depth, nodes_mb);

    // Force the loop-based slider path and time the bitboard.
    reset_magic_attacks();
    Attacks::set_rook_attack_fn(&Attacks::rook_loop);
    Attacks::set_bishop_attack_fn(&Attacks::bishop_loop);
    std::uint64_t nodes_loop = 0;
    double t_loop = 0;
    time_bb(p.fen, depth, nodes_loop, t_loop);

    // Switch to magic (file / generate) and time again.
    double t_magic = 0;
    std::uint64_t nodes_magic = 0;
    bool has_magic =
        init_magic_attacks_from_default_locations() || init_magic_attacks();
    if (has_magic) {
        time_bb(p.fen, depth, nodes_magic, t_magic);
    }

    // Switch to PEXT (if compiled and CPU supports it) and
    // time again. Init returns false on non-PEXT builds — the
    // block then stays unevaluated.
    double t_pext = 0;
    std::uint64_t nodes_pext = 0;
    const bool has_pext = init_pext_attacks();
    if (has_pext) {
        time_bb(p.fen, depth, nodes_pext, t_pext);
    }

    std::printf("%-9s d=%d  n=%12llu\n", p.name.data(), depth,
                static_cast<unsigned long long>(nodes_mb));
    std::printf("  mailbox        %6.2fs  %5.2f Mnps\n",
                t_mb, mnps_of(nodes_mb, t_mb));
    std::printf("  bb loop        %6.2fs  %5.2f Mnps  (x%.2f vs mailbox)\n",
                t_loop, mnps_of(nodes_loop, t_loop), t_mb / t_loop);
    if (has_magic) {
        std::printf("  bb magic       %6.2fs  %5.2f Mnps  (x%.2f vs mailbox)\n",
                    t_magic, mnps_of(nodes_magic, t_magic), t_mb / t_magic);
    }
    if (has_pext) {
        std::printf("  bb pext(BMI2)  %6.2fs  %5.2f Mnps  (x%.2f vs mailbox)\n",
                    t_pext, mnps_of(nodes_pext, t_pext), t_mb / t_pext);
    }

    // Node-count sanity check.
    if (nodes_loop != nodes_mb ||
        (has_magic && nodes_magic != nodes_mb) ||
        (has_pext  && nodes_pext  != nodes_mb)) {
        std::fprintf(stderr, "  WARNING: node-count mismatch across paths\n");
    }
}

} // namespace

int main(int argc, char** argv) {
    int depth_override = -1;
    std::string_view only;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-h" || a == "--help") {
            print_help();
            return 0;
        }
        if (a == "--depth") {
            if (i + 1 >= argc) {
                std::fputs("--depth requires a value\n", stderr);
                return 2;
            }
            depth_override = std::atoi(argv[++i]);
            if (depth_override < 0) {
                std::fputs("--depth must be non-negative\n", stderr);
                return 2;
            }
            continue;
        }
        if (a == "--only") {
            if (i + 1 >= argc) {
                std::fputs("--only requires a value\n", stderr);
                return 2;
            }
            only = argv[++i];
            continue;
        }
        std::fprintf(stderr, "unknown option '%.*s'\n",
                     static_cast<int>(a.size()), a.data());
        print_help();
        return 2;
    }

    bool ran = false;
    for (const BenchPosition& p : POSITIONS) {
        if (!only.empty() && only != p.name) {
            continue;
        }
        const int d = (depth_override >= 0) ? depth_override : p.default_depth;
        run_one(p, d);
        ran = true;
    }
    if (!ran) {
        std::fprintf(stderr, "no position matched '--only %.*s'\n",
                     static_cast<int>(only.size()), only.data());
        return 2;
    }
    return 0;
}

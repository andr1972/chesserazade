// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// PEXT slider attacks.
///
/// Uses the x86 `PEXT` (parallel bits extract) instruction
/// from BMI2 to extract the relevant-occupancy bits directly
/// into a contiguous index. No magic multiply needed:
///
///     index = pext(occ, mask)
///     attacks = table[index]
///
/// On Intel Haswell+ and AMD Zen 3+ this is a single-cycle
/// instruction. On older AMD Zen (1/2), PEXT is microcoded
/// and slow enough that magic bitboards are typically faster
/// — which is why `init_pext_attacks()` still gates on the
/// `bmi2` CPU feature at runtime even though the code was
/// compiled for it.
///
/// This TU is compiled only when `CHESSERAZADE_USE_PEXT` is
/// defined (CMake option). Without it, the stub
/// `init_pext_attacks()` in magic.cpp just returns false and
/// the priority chain falls through to magic / loop.

#include "board/magic.hpp"

#include <immintrin.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace chesserazade {

namespace {

/// Per-square PEXT entries. We reuse the `MagicEntry` struct
/// for the shape, ignoring `magic` and `shift` (PEXT doesn't
/// need them). `mask` and `attacks_offset` are meaningful.
std::array<MagicEntry, NUM_SQUARES> g_pext_rook_entries{};
std::array<MagicEntry, NUM_SQUARES> g_pext_bishop_entries{};
std::vector<Bitboard> g_pext_rook_table;
std::vector<Bitboard> g_pext_bishop_table;
bool g_pext_ready = false;

[[nodiscard]] Bitboard pext_rook(Square sq, Bitboard occ) noexcept {
    const MagicEntry& e = g_pext_rook_entries[to_index(sq)];
    const std::uint64_t idx = _pext_u64(occ, e.mask);
    return g_pext_rook_table[e.attacks_offset + idx];
}

[[nodiscard]] Bitboard pext_bishop(Square sq, Bitboard occ) noexcept {
    const MagicEntry& e = g_pext_bishop_entries[to_index(sq)];
    const std::uint64_t idx = _pext_u64(occ, e.mask);
    return g_pext_bishop_table[e.attacks_offset + idx];
}

/// Build the per-square table slice indexed by PEXT. Because
/// `pext(index_to_occupancy(i, mask), mask) == i` by
/// construction, we can fill the table in index order rather
/// than computing a hash — one less dependency on
/// `pext_u64` during init.
void build_pext_slice(Square sq, bool is_rook, MagicEntry& e,
                      std::vector<Bitboard>& table) {
    const int n = std::popcount(e.mask);
    const std::size_t size = std::size_t{1} << n;
    const std::size_t offset = table.size();
    table.resize(offset + size);
    for (std::size_t i = 0; i < size; ++i) {
        const Bitboard occ =
            index_to_occupancy(static_cast<int>(i), e.mask);
        table[offset + i] =
            is_rook ? Attacks::rook_loop(sq, occ)
                    : Attacks::bishop_loop(sq, occ);
    }
    e.attacks_offset = offset;
}

} // namespace

bool init_pext_attacks() {
    if (g_pext_ready) return true;

    // Runtime check: even if compiled in, the current CPU may
    // not support the instruction. Older x86 without BMI2
    // would SIGILL on the first `pext`.
    if (!__builtin_cpu_supports("bmi2")) return false;

    g_pext_rook_table.clear();
    g_pext_bishop_table.clear();

    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Square sq = static_cast<Square>(i);
        g_pext_rook_entries[i].mask = rook_relevant_mask(sq);
        build_pext_slice(sq, /*is_rook=*/true,
                         g_pext_rook_entries[i], g_pext_rook_table);
        g_pext_bishop_entries[i].mask = bishop_relevant_mask(sq);
        build_pext_slice(sq, /*is_rook=*/false,
                         g_pext_bishop_entries[i], g_pext_bishop_table);
    }

    Attacks::set_rook_attack_fn(&pext_rook);
    Attacks::set_bishop_attack_fn(&pext_bishop);
    g_pext_ready = true;
    return true;
}

bool pext_attacks_available() noexcept { return g_pext_ready; }

} // namespace chesserazade

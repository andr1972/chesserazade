// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Magic bitboards — O(1) slider attack lookup.
///
/// The idea (Vlastimil Sejvl / Pradu Kannan, ca. 2006): for a
/// rook on square `s`, its attack set depends only on the
/// occupancy bits along its four rays. That is at most 14 bits
/// of information; after clipping the edges (they cannot block
/// since a piece on the edge is the end of the ray anyway) it
/// is at most 10–12 bits. We want to hash those scattered bits
/// into a small index to look up a precomputed attack table.
///
/// A **magic number** `M[s]` is a 64-bit constant such that
///
///     ((occupancy & mask[s]) * M[s]) >> shift[s]
///
/// maps every possible occupancy (of which there are `2^n` for
/// `n = popcount(mask[s])`) to a distinct index in `[0, 2^n)`.
/// Finding such a constant is done offline by trial-and-error:
/// sample candidate 64-bit values with a low Hamming density
/// until one perfectly hashes all occupancies with no collision.
///
/// Runtime cost per lookup: one AND, one MUL, one shift, one
/// load. Regardless of distance to the nearest blocker. That is
/// the speedup over the O(distance) ray walk in
/// `Attacks::rook_loop` / `bishop_loop`.
///
/// This header is internal to `src/board/`. Public code keeps
/// using `Attacks::rook(sq, occ)` / `Attacks::bishop(sq, occ)`;
/// `init_magic_attacks()` swaps the global function pointer so
/// those calls reach the magic path transparently.
///
/// Reference: https://www.chessprogramming.org/Magic_Bitboards
#pragma once

#include <chesserazade/bitboard.hpp>
#include <chesserazade/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace chesserazade {

/// Per-square runtime info for a single slider (rook or
/// bishop). The attack tables sit in a shared contiguous
/// buffer; `attacks_offset` is the starting index for this
/// square, and `shift` is `64 - popcount(mask)`.
struct MagicEntry {
    Bitboard  mask           = 0;
    std::uint64_t magic      = 0;
    unsigned  shift          = 0;
    std::size_t attacks_offset = 0;
};

// Slider-table helpers shared between `magic.cpp` and the
// optional `attacks_pext.cpp`. Private to `src/board/` —
// public callers still go through `Attacks::rook` / bishop.
[[nodiscard]] Bitboard rook_relevant_mask(Square sq) noexcept;
[[nodiscard]] Bitboard bishop_relevant_mask(Square sq) noexcept;
[[nodiscard]] Bitboard index_to_occupancy(int index, Bitboard mask) noexcept;

/// Initialize the global magic tables by brute-force search
/// and swap `Attacks::rook/bishop` to the magic lookup.
/// Idempotent; typical cost < 100 ms. Returns true on success.
bool init_magic_attacks();

/// Same as `init_magic_attacks()` but populates the tables
/// from the magic constants listed in `path` instead of
/// searching. Faster on cold start (~ms vs ~100 ms) and
/// deterministic. Returns true on success; false if the file
/// is missing, malformed, or contains a constant that fails
/// the collision-free check (which would mean the file is
/// corrupted or hand-edited incorrectly).
[[nodiscard]] bool init_magic_attacks_from_file(const std::string& path);

/// Walk the standard lookup chain and initialize from the
/// first file found:
///
///    $CHESSERAZADE_MAGICS         (explicit override)
///    $ORIGIN/data/magics.txt      (binary's sibling data dir)
///    $ORIGIN/../data/magics.txt
///    $ORIGIN/../../data/magics.txt  (works from build/<preset>/)
///    <CMAKE_SOURCE_DIR>/data/magics.txt  (baked absolute path)
///
/// Returns true if any path loads successfully. Does NOT fall
/// back to brute-force generation — that is an explicit
/// opt-in via `init_magic_attacks()` or the `magics-gen` CLI.
[[nodiscard]] bool init_magic_attacks_from_default_locations();

/// Serialize the current magic constants + masks + shifts to
/// `path` in the human-readable format consumed by
/// `init_magic_attacks_from_file`. Precondition: magics are
/// initialized. Returns true on successful write.
[[nodiscard]] bool write_magics_to_file(const std::string& path);

/// True once any `init_magic_attacks*` call has succeeded.
[[nodiscard]] bool magic_attacks_available() noexcept;

/// Reset to the uninitialized state — used by tests that want
/// to exercise a different init path within the same process.
void reset_magic_attacks() noexcept;

// ---------------------------------------------------------------------------
// PEXT slider attacks (optional, BMI2-only)
// ---------------------------------------------------------------------------
//
// Compiled only when the CMake option CHESSERAZADE_USE_PEXT is
// ON and the target CPU supports BMI2. Uses the x86 `PEXT`
// instruction to extract the relevant occupancy bits into a
// contiguous index — one `pext` + one load per lookup, no magic
// multiply needed. Slightly faster than magic on hardware where
// PEXT is native (Intel Haswell+, AMD Zen 3+); notably slow on
// pre-Zen-3 AMD where PEXT is microcoded.
//
// The two init functions below are stubs on non-PEXT builds —
// they always return false, and the caller falls back to the
// magic / loop path.

/// Initialize PEXT-based slider attacks. Returns false if:
///   * the build does not include PEXT support, or
///   * the CPU does not advertise the BMI2 feature bit.
/// On success swaps `Attacks::rook / bishop` to the PEXT path.
[[nodiscard]] bool init_pext_attacks();

/// True once `init_pext_attacks()` has succeeded.
[[nodiscard]] bool pext_attacks_available() noexcept;

// ---------------------------------------------------------------------------
// Unified init entry
// ---------------------------------------------------------------------------

/// Initialize the fastest slider-attack implementation this
/// build + CPU supports. Priority:
///   1. PEXT (if compiled in and CPU supports BMI2).
///   2. Magic bitboards loaded from a `data/magics.txt` found
///      via the standard lookup chain.
///   3. Magic bitboards generated in memory (brute-force,
///      ~100 ms).
///   4. Loop-based — no init required; the default pointer
///      already points here.
///
/// Returns the name of the selected path as one of:
///   "pext" | "magic-file" | "magic-gen" | "loop"
[[nodiscard]] std::string_view init_slider_attacks();

} // namespace chesserazade

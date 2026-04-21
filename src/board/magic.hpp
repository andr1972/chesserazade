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

/// Initialize the global magic tables and swap
/// `Attacks::rook/bishop` to use the magic lookup. Safe to
/// call more than once (idempotent). Runs a brute-force search
/// for magic constants in memory; typical cost < 100 ms on a
/// modern machine.
///
/// Returns true on success. On failure (unable to find a magic
/// for some square — statistically extremely unlikely with the
/// search parameters used here), the dispatch stays on the
/// loop-based baseline and returns false.
bool init_magic_attacks();

/// True once `init_magic_attacks()` has succeeded.
[[nodiscard]] bool magic_attacks_available() noexcept;

} // namespace chesserazade

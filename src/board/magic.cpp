/// Magic bitboards implementation.
///
/// Three phases on init:
///   1. Build the relevant-occupancy mask for each square
///      (edges removed — they can't block).
///   2. Brute-force a magic constant for each square that
///      maps every occupancy subset of that mask to a distinct
///      index. We sample 64-bit random candidates with low
///      Hamming density (bitwise AND of three random words —
///      a trick that biases towards few set bits, which tend
///      to produce collision-free hashes more quickly).
///   3. Fill a shared attack table, one entry per (square,
///      occupancy subset), indexed by the magic hash.
///
/// Runtime lookup is then a single multiply + shift + load.
///
/// We keep the loop-based reference alongside the magic path
/// so we can verify the tables during init: for every square
/// and every occupancy subset we compute the expected attack
/// set with the classical ray walk and check that the magic
/// index lands there. This guarantees any broken magic fails
/// loudly *at program start* rather than producing silently
/// wrong moves hours later.

#include "board/magic.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace chesserazade {

namespace {

// ---------------------------------------------------------------------------
// Data layout
// ---------------------------------------------------------------------------

/// Global per-square magic entries and the shared attack table.
/// Kept at namespace scope; `init_magic_attacks()` populates
/// them. The attack table is a `std::vector<Bitboard>` so its
/// lifetime matches the singleton; no static-destruction
/// ordering issues because the only consumers (Attacks::rook /
/// bishop) are destroyed at the same phase.
std::array<MagicEntry, NUM_SQUARES> g_rook_entries{};
std::array<MagicEntry, NUM_SQUARES> g_bishop_entries{};
std::vector<Bitboard> g_rook_table;
std::vector<Bitboard> g_bishop_table;
bool g_ready = false;

// ---------------------------------------------------------------------------
// Ray walk (reference) — used both to build the target attacks
// for a given occupancy and to sanity-check the magic table
// during init. Duplicates the private helper in bitboard.cpp
// because we need it at file scope here.
// ---------------------------------------------------------------------------

[[nodiscard]] Bitboard ray_to(Square from, int dr, int df,
                              Bitboard occ) noexcept {
    Bitboard result = 0;
    int r = static_cast<int>(rank_of(from)) + dr;
    int f = static_cast<int>(file_of(from)) + df;
    while (r >= 0 && r < 8 && f >= 0 && f < 8) {
        const Bitboard m = Bitboard{1} << (r * 8 + f);
        result |= m;
        if (occ & m) break;
        r += dr; f += df;
    }
    return result;
}

[[nodiscard]] Bitboard rook_ref(Square sq, Bitboard occ) noexcept {
    return ray_to(sq, +1, 0, occ) | ray_to(sq, -1, 0, occ)
         | ray_to(sq, 0, +1, occ) | ray_to(sq, 0, -1, occ);
}
[[nodiscard]] Bitboard bishop_ref(Square sq, Bitboard occ) noexcept {
    return ray_to(sq, +1, +1, occ) | ray_to(sq, +1, -1, occ)
         | ray_to(sq, -1, +1, occ) | ray_to(sq, -1, -1, occ);
}

// ---------------------------------------------------------------------------
// Relevant-occupancy masks
// ---------------------------------------------------------------------------
//
// The "relevant" squares are the ones a slider can walk through
// *before* reaching the edge. A piece sitting on the edge can't
// be "in the way" for anything beyond, so we exclude edge files
// (a and h) from horizontal rays and edge ranks (1 and 8) from
// vertical rays. For bishops, both edge ranks and both edge
// files are excluded on every ray.

[[nodiscard]] Bitboard rook_relevant_mask(Square sq) noexcept {
    const int r = static_cast<int>(rank_of(sq));
    const int f = static_cast<int>(file_of(sq));
    Bitboard m = 0;
    // North (exclude rank 8).
    for (int nr = r + 1; nr < 7; ++nr) m |= Bitboard{1} << (nr * 8 + f);
    // South (exclude rank 1).
    for (int nr = r - 1; nr > 0; --nr) m |= Bitboard{1} << (nr * 8 + f);
    // East (exclude file h).
    for (int nf = f + 1; nf < 7; ++nf) m |= Bitboard{1} << (r * 8 + nf);
    // West (exclude file a).
    for (int nf = f - 1; nf > 0; --nf) m |= Bitboard{1} << (r * 8 + nf);
    return m;
}

[[nodiscard]] Bitboard bishop_relevant_mask(Square sq) noexcept {
    const int r = static_cast<int>(rank_of(sq));
    const int f = static_cast<int>(file_of(sq));
    Bitboard m = 0;
    for (int nr = r + 1, nf = f + 1; nr < 7 && nf < 7; ++nr, ++nf)
        m |= Bitboard{1} << (nr * 8 + nf);
    for (int nr = r + 1, nf = f - 1; nr < 7 && nf > 0; ++nr, --nf)
        m |= Bitboard{1} << (nr * 8 + nf);
    for (int nr = r - 1, nf = f + 1; nr > 0 && nf < 7; --nr, ++nf)
        m |= Bitboard{1} << (nr * 8 + nf);
    for (int nr = r - 1, nf = f - 1; nr > 0 && nf > 0; --nr, --nf)
        m |= Bitboard{1} << (nr * 8 + nf);
    return m;
}

// ---------------------------------------------------------------------------
// Occupancy subset enumeration
// ---------------------------------------------------------------------------
//
// Given a mask of `n` set bits, enumerate all `2^n` subsets by
// "spreading" an integer `0..(2^n-1)` across the mask's bit
// positions. Classical pdep-style trick without the intrinsic.

[[nodiscard]] Bitboard index_to_occupancy(int index, Bitboard mask) noexcept {
    Bitboard occ = 0;
    int bit = 0;
    Bitboard m = mask;
    while (m) {
        const Square s = pop_lsb(m);
        if (index & (1 << bit)) {
            occ |= bb_of(s);
        }
        ++bit;
    }
    return occ;
}

// ---------------------------------------------------------------------------
// Magic finder
// ---------------------------------------------------------------------------

/// Low-density random 64-bit candidate: the bitwise AND of
/// three `mt19937_64` draws. Sparse candidates tend to produce
/// collision-free magics much faster than uniform ones.
[[nodiscard]] std::uint64_t sparse_random(std::mt19937_64& rng) noexcept {
    return rng() & rng() & rng();
}

/// Try to find a magic for `mask` that maps all `2^n`
/// occupancy subsets to distinct indices; on success populate
/// `out` and fill the slice of `table` at
/// `[out.attacks_offset, out.attacks_offset + 2^n)`. Returns
/// true on success, false if the trial budget was exhausted.
bool find_magic_for(Square sq, Bitboard mask,
                    bool is_rook,
                    MagicEntry& out,
                    std::vector<Bitboard>& table) {
    const int n = std::popcount(mask);
    const std::size_t size = std::size_t{1} << n;
    const std::size_t offset = table.size();
    table.resize(offset + size);

    // Precompute (occupancy, reference-attack) pairs so the
    // trial loop doesn't re-compute them per candidate.
    std::vector<Bitboard> occs(size);
    std::vector<Bitboard> refs(size);
    for (std::size_t i = 0; i < size; ++i) {
        occs[i] = index_to_occupancy(static_cast<int>(i), mask);
        refs[i] = is_rook ? rook_ref(sq, occs[i])
                          : bishop_ref(sq, occs[i]);
    }

    // Per-rank seeds known from CPW / Stockfish to find magics
    // quickly for every square. We XOR with the file so the
    // per-square sequences are distinct within a rank.
    constexpr std::uint64_t RANK_SEEDS[8] = {
        728ULL,   10316ULL, 55013ULL, 32803ULL,
        12281ULL, 15100ULL, 16645ULL, 255ULL,
    };
    const int rank_i = static_cast<int>(rank_of(sq));
    const int file_i = static_cast<int>(file_of(sq));
    std::mt19937_64 rng(RANK_SEEDS[rank_i]
                        + static_cast<std::uint64_t>(file_i));

    std::vector<Bitboard> used(size);
    for (int trial = 0; trial < 10'000'000; ++trial) {
        const std::uint64_t magic = sparse_random(rng);
        // Fast skip: require many high bits in (mask * magic).
        // Classical heuristic from CPW; avoids obviously-bad
        // candidates.
        if (std::popcount((mask * magic) & 0xFF00'0000'0000'0000ULL) < 6)
            continue;

        std::fill(used.begin(), used.end(), Bitboard{0});
        bool ok = true;
        for (std::size_t i = 0; i < size; ++i) {
            const std::uint64_t index =
                (occs[i] * magic) >> (64 - n);
            if (used[index] == 0) {
                used[index] = refs[i];
            } else if (used[index] != refs[i]) {
                // Collision with a different attack set.
                ok = false;
                break;
            }
            // used[index] == refs[i] is a "constructive
            // collision" — same attack set, different
            // occupancy; harmless, we can share the slot.
        }
        if (!ok) continue;

        // Success — record and fill the table slice.
        out.mask = mask;
        out.magic = magic;
        out.shift = static_cast<unsigned>(64 - n);
        out.attacks_offset = offset;
        for (std::size_t i = 0; i < size; ++i) {
            const std::uint64_t index =
                (occs[i] * magic) >> out.shift;
            table[offset + index] = refs[i];
        }
        return true;
    }
    std::fprintf(stderr,
                 "magic bitboards: failed to find magic for square %d (%s)\n",
                 static_cast<int>(sq), is_rook ? "rook" : "bishop");
    return false;
}

// ---------------------------------------------------------------------------
// Attack lookup
// ---------------------------------------------------------------------------

[[nodiscard]] Bitboard magic_rook(Square sq, Bitboard occ) noexcept {
    const MagicEntry& e = g_rook_entries[to_index(sq)];
    const std::uint64_t index = ((occ & e.mask) * e.magic) >> e.shift;
    return g_rook_table[e.attacks_offset + index];
}

[[nodiscard]] Bitboard magic_bishop(Square sq, Bitboard occ) noexcept {
    const MagicEntry& e = g_bishop_entries[to_index(sq)];
    const std::uint64_t index = ((occ & e.mask) * e.magic) >> e.shift;
    return g_bishop_table[e.attacks_offset + index];
}

} // namespace

bool init_magic_attacks() {
    if (g_ready) return true;

    g_rook_table.clear();
    g_bishop_table.clear();

    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Square sq = static_cast<Square>(i);
        if (!find_magic_for(sq, rook_relevant_mask(sq), true,
                            g_rook_entries[i], g_rook_table)) {
            return false;
        }
        if (!find_magic_for(sq, bishop_relevant_mask(sq), false,
                            g_bishop_entries[i], g_bishop_table)) {
            return false;
        }
    }

    Attacks::set_rook_attack_fn(&magic_rook);
    Attacks::set_bishop_attack_fn(&magic_bishop);
    g_ready = true;
    return true;
}

bool magic_attacks_available() noexcept { return g_ready; }

} // namespace chesserazade

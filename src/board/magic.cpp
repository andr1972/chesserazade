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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
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

void reset_magic_attacks() noexcept {
    g_rook_entries = {};
    g_bishop_entries = {};
    g_rook_table.clear();
    g_bishop_table.clear();
    g_ready = false;
    Attacks::set_rook_attack_fn(&Attacks::rook_loop);
    Attacks::set_bishop_attack_fn(&Attacks::bishop_loop);
}

// ---------------------------------------------------------------------------
// File format
// ---------------------------------------------------------------------------
//
//   # chesserazade magic bitboards
//   # format: section square magic(hex) mask(hex) shift(dec)
//   [rook]
//   a1 0x... 0x... 52
//   ...
//   [bishop]
//   a1 0x... 0x... 58
//   ...
//
// Attack tables are not in the file — they are derived on
// load from (magic, mask, shift). `load` validates every
// (square, occupancy-subset) maps to exactly one attack set,
// so a hand-edited / corrupted file is rejected, not blindly
// trusted.

namespace {

/// Populate `out.attacks_offset` and the attack-table slice
/// at that offset using the already-set (mask, magic, shift).
/// Returns false if the magic is not collision-free (which
/// means we should reject the loaded file).
bool build_table_slice(Square sq, bool is_rook, MagicEntry& out,
                       std::vector<Bitboard>& table) {
    const int n = std::popcount(out.mask);
    const std::size_t size = std::size_t{1} << n;
    const std::size_t offset = table.size();
    table.resize(offset + size);

    std::vector<Bitboard> used(size);
    for (std::size_t i = 0; i < size; ++i) {
        const Bitboard occ =
            index_to_occupancy(static_cast<int>(i), out.mask);
        const Bitboard ref = is_rook ? rook_ref(sq, occ) : bishop_ref(sq, occ);
        const std::uint64_t idx = (occ * out.magic) >> out.shift;
        if (used[idx] == 0) {
            used[idx] = ref;
            table[offset + idx] = ref;
        } else if (used[idx] != ref) {
            // Collision with a different attack set — the
            // supplied magic is invalid for this square.
            return false;
        }
    }
    out.attacks_offset = offset;
    return true;
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'
                          || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'
                          || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

[[nodiscard]] bool parse_hex64(std::string_view s, std::uint64_t& out) noexcept {
    if (s.starts_with("0x") || s.starts_with("0X")) s.remove_prefix(2);
    if (s.empty() || s.size() > 16) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9')       v |= static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f')  v |= static_cast<std::uint64_t>(10 + (c - 'a'));
        else if (c >= 'A' && c <= 'F')  v |= static_cast<std::uint64_t>(10 + (c - 'A'));
        else return false;
    }
    out = v;
    return true;
}

[[nodiscard]] bool parse_square(std::string_view s, Square& out) noexcept {
    if (s.size() != 2) return false;
    if (s[0] < 'a' || s[0] > 'h') return false;
    if (s[1] < '1' || s[1] > '8') return false;
    out = make_square(static_cast<File>(s[0] - 'a'),
                      static_cast<Rank>(s[1] - '1'));
    return true;
}

} // namespace

bool init_magic_attacks_from_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    reset_magic_attacks();

    enum class Section { None, Rook, Bishop };
    Section section = Section::None;

    std::string line;
    while (std::getline(f, line)) {
        std::string_view s = trim(line);
        if (s.empty()) continue;
        if (s.front() == '#') continue;
        if (s == "[rook]")   { section = Section::Rook;   continue; }
        if (s == "[bishop]") { section = Section::Bishop; continue; }

        // Expect: <square> <magic_hex> <mask_hex> <shift_dec>
        std::istringstream is{std::string{s}};
        std::string sq_tok, magic_tok, mask_tok;
        int shift = 0;
        if (!(is >> sq_tok >> magic_tok >> mask_tok >> shift)) {
            reset_magic_attacks();
            return false;
        }

        Square sq{};
        std::uint64_t magic = 0, mask = 0;
        if (!parse_square(sq_tok, sq) || !parse_hex64(magic_tok, magic)
            || !parse_hex64(mask_tok, mask) || shift < 0 || shift > 63) {
            reset_magic_attacks();
            return false;
        }

        MagicEntry entry;
        entry.mask = mask;
        entry.magic = magic;
        entry.shift = static_cast<unsigned>(shift);

        std::vector<Bitboard>& table =
            (section == Section::Rook) ? g_rook_table : g_bishop_table;
        const bool is_rook = (section == Section::Rook);

        if (section == Section::None) {
            reset_magic_attacks();
            return false;
        }

        // Validate the mask matches what the square demands
        // (defends against file corruption).
        const Bitboard expected_mask =
            is_rook ? rook_relevant_mask(sq) : bishop_relevant_mask(sq);
        if (mask != expected_mask) {
            reset_magic_attacks();
            return false;
        }

        if (!build_table_slice(sq, is_rook, entry, table)) {
            reset_magic_attacks();
            return false;
        }
        auto& arr = is_rook ? g_rook_entries : g_bishop_entries;
        arr[to_index(sq)] = entry;
    }

    // Sanity: every square in both sections must have been set.
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        if (g_rook_entries[i].magic == 0
            || g_bishop_entries[i].magic == 0) {
            reset_magic_attacks();
            return false;
        }
    }

    Attacks::set_rook_attack_fn(&magic_rook);
    Attacks::set_bishop_attack_fn(&magic_bishop);
    g_ready = true;
    return true;
}

namespace {
void emit_magic_section(
    std::ofstream& f, const char* name,
    const std::array<MagicEntry, NUM_SQUARES>& arr) {
    f << "[" << name << "]\n";
    for (std::uint8_t i = 0; i < NUM_SQUARES; ++i) {
        const Square sq = static_cast<Square>(i);
        const MagicEntry& e = arr[i];
        const char sq_buf[3] = {
            static_cast<char>('a' + static_cast<int>(file_of(sq))),
            static_cast<char>('1' + static_cast<int>(rank_of(sq))),
            '\0'
        };
        char line[128];
        std::snprintf(line, sizeof(line),
                      "%s 0x%016lx 0x%016lx %u\n",
                      sq_buf,
                      static_cast<unsigned long>(e.magic),
                      static_cast<unsigned long>(e.mask),
                      e.shift);
        f << line;
    }
    f << '\n';
}
} // namespace

bool write_magics_to_file(const std::string& path) {
    if (!g_ready) return false;
    std::ofstream f(path);
    if (!f) return false;

    f << "# chesserazade magic bitboards\n"
      << "# format: section square magic(hex) mask(hex) shift(dec)\n"
      << "# generated by: chesserazade magics-gen\n\n";

    emit_magic_section(f, "rook",   g_rook_entries);
    emit_magic_section(f, "bishop", g_bishop_entries);
    return f.good();
}

namespace {

/// Return the directory the current executable lives in, or
/// empty on error. Linux-specific (`readlink /proc/self/exe`).
[[nodiscard]] std::filesystem::path binary_dir() noexcept {
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return p.parent_path();
}

/// Compile-time baked path: the repo-root `data/magics.txt`
/// so a developer-built binary always works without setting
/// the env var.
[[nodiscard]] std::filesystem::path baked_default_path() noexcept {
#ifdef CHESSERAZADE_SOURCE_DIR
    return std::filesystem::path(CHESSERAZADE_SOURCE_DIR) / "data" / "magics.txt";
#else
    return {};
#endif
}

} // namespace

bool init_magic_attacks_from_default_locations() {
    namespace fs = std::filesystem;

    // 1. Explicit env var override.
    if (const char* env = std::getenv("CHESSERAZADE_MAGICS")) {
        if (env[0] != '\0' && init_magic_attacks_from_file(env)) {
            return true;
        }
    }

    // 2–4. Relative to the binary.
    const fs::path bd = binary_dir();
    if (!bd.empty()) {
        const fs::path candidates[] = {
            bd / "data" / "magics.txt",
            bd / ".." / "data" / "magics.txt",
            bd / ".." / ".." / "data" / "magics.txt",
        };
        for (const fs::path& rel : candidates) {
            std::error_code ec;
            if (fs::exists(rel, ec)
                && init_magic_attacks_from_file(rel.string())) {
                return true;
            }
        }
    }

    // 5. Baked absolute path from the CMake source dir.
    const fs::path baked = baked_default_path();
    if (!baked.empty()) {
        std::error_code ec;
        if (fs::exists(baked, ec)
            && init_magic_attacks_from_file(baked.string())) {
            return true;
        }
    }

    return false;
}

} // namespace chesserazade

/// Transposition table — cache of search results keyed by
/// Zobrist.
///
/// Why: the search visits many positions more than once —
/// different move orders often transpose into the same node, and
/// iterative deepening re-visits the whole top of the tree on
/// every new depth. Caching the result of a sub-tree by its
/// Zobrist key lets us skip the work on re-encounter.
///
/// Entry contents:
///   * `key`    — full 64-bit Zobrist, so the probe can verify
///                it got the right entry (we use type-1 collision
///                detection: key match == trusted).
///   * `depth`  — the *remaining* search depth the stored score
///                was computed at. A shallower entry cannot
///                answer a deeper question.
///   * `score`  — the value, possibly a bound (see `flags`).
///                Mate scores are stored in a ply-relative form
///                so they transplant correctly when the same
///                position is seen at a different root distance.
///   * `flags`  — low 2 bits encode the bound type:
///                `Exact`     — exact minimax value,
///                `Lower`     — at least this score (fail-high),
///                `Upper`     — at most this score (fail-low).
///                High 6 bits are the age, incremented once per
///                root `find_best` call; a stale entry from a
///                previous search is safe to overwrite.
///   * `move`   — best move for this position, used as the
///                first-tried move by 0.8's ordering pass.
///
/// Replacement scheme: on a collision, the new entry replaces
/// the old one iff
///   (new_depth >= old_depth)  OR  (new_age != old_age).
/// In plain English: prefer fresher searches (age wins) and,
/// within an age, prefer the deeper analysis.
///
/// The table is *not* cleared between calls to `find_best` —
/// that's the whole point. It *is* cleared on user-visible
/// events like loading a new position in the REPL, where the
/// cached entries from one game cannot help analysis of another.
///
/// Reference: https://www.chessprogramming.org/Transposition_Table
#pragma once

#include <chesserazade/board.hpp>
#include <chesserazade/move.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace chesserazade {

enum class TtBound : std::uint8_t {
    None  = 0,
    Exact = 1,
    Lower = 2,
    Upper = 3,
};

struct TtEntry {
    ZobristKey key = 0;
    Move move{};
    std::int16_t score = 0;
    std::uint8_t depth = 0;
    /// Low 2 bits: `TtBound`. High 6 bits: age, 0..63 (wraps).
    std::uint8_t flags = 0;

    [[nodiscard]] TtBound bound() const noexcept {
        return static_cast<TtBound>(flags & 0b11u);
    }
    [[nodiscard]] std::uint8_t age() const noexcept {
        return static_cast<std::uint8_t>(flags >> 2);
    }
    [[nodiscard]] bool empty() const noexcept { return bound() == TtBound::None; }
};

/// Result of a successful probe — the stored entry is copied out
/// by value so the caller does not have to keep the table
/// pointer alive across recursive search calls.
struct TtProbe {
    bool hit = false;
    TtEntry entry{};
};

class TranspositionTable {
public:
    /// Allocate a table with approximately `n_entries` slots.
    /// The effective size is rounded down to the nearest power
    /// of two so indexing is a cheap AND. The default (1 << 20 =
    /// ~1M entries × 16B ≈ 16 MiB) suits the 0.x-era searches.
    explicit TranspositionTable(std::size_t n_entries = 1u << 20);

    /// Clear all entries. Called when the search context changes
    /// (e.g. a new position is loaded in the REPL).
    void clear() noexcept;

    /// Start of a new search root. Increments the age counter so
    /// the next batch of stores supersede entries from the
    /// previous call.
    void new_search() noexcept;

    /// Probe the table by position key.
    [[nodiscard]] TtProbe probe(ZobristKey key) const noexcept;

    /// Store an entry. `depth` is the *remaining* depth at which
    /// `score` was computed; `bound` describes how the score
    /// relates to the true value; `move` is the best move found
    /// (or default if unknown).
    void store(ZobristKey key, int depth, int score, TtBound bound,
               Move move) noexcept;

    /// Statistics — updated by probe / store. Not synchronized;
    /// intended for single-threaded inspection.
    [[nodiscard]] std::uint64_t probes() const noexcept { return probes_; }
    [[nodiscard]] std::uint64_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::uint64_t stores() const noexcept { return stores_; }

    /// Size in entries (a power of two).
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<TtEntry> entries_;
    std::size_t mask_ = 0;
    std::uint8_t age_ = 0;

    /// Statistics. Public via the accessors above; mutable here
    /// because `probe` is logically const but bumps `probes_` /
    /// `hits_`.
    mutable std::uint64_t probes_ = 0;
    mutable std::uint64_t hits_   = 0;
    std::uint64_t stores_ = 0;
};

} // namespace chesserazade

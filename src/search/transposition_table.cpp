// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include <chesserazade/transposition_table.hpp>

#include <algorithm>
#include <cstdint>

namespace chesserazade {

namespace {

/// Round `n` down to the nearest power of two; return at least 1.
/// A power-of-two table size lets us use a bitmask instead of `%`
/// for index mapping, which is a few cycles cheaper per probe.
[[nodiscard]] std::size_t floor_pow2(std::size_t n) noexcept {
    if (n == 0) return 1;
    std::size_t p = 1;
    while ((p << 1) <= n) p <<= 1;
    return p;
}

} // namespace

TranspositionTable::TranspositionTable(std::size_t n_entries) {
    const std::size_t sz = floor_pow2(std::max<std::size_t>(n_entries, 1));
    entries_.assign(sz, TtEntry{});
    mask_ = sz - 1;
}

void TranspositionTable::clear() noexcept {
    std::fill(entries_.begin(), entries_.end(), TtEntry{});
    probes_ = hits_ = stores_ = 0;
    age_ = 0;
}

void TranspositionTable::new_search() noexcept {
    // 6-bit field, wraps at 64. On wrap, old entries look "newer"
    // than current — in practice this is rare and the
    // depth-part of the replacement criterion still catches it.
    age_ = static_cast<std::uint8_t>((age_ + 1) & 0b11'1111u);
}

TtProbe TranspositionTable::probe(ZobristKey key) const noexcept {
    ++probes_;
    const TtEntry& e = entries_[key & mask_];
    if (e.bound() != TtBound::None && e.key == key) {
        ++hits_;
        return {true, e};
    }
    return {false, {}};
}

void TranspositionTable::store(ZobristKey key, int depth, int score,
                               TtBound bound, Move move) noexcept {
    ++stores_;
    TtEntry& slot = entries_[key & mask_];

    // Replacement: new entry wins if the slot is empty, the keys
    // match (refresh), the slot has stale age, or the new depth
    // is at least as deep. See class doc-block for the rationale.
    const bool replace =
        slot.empty()
        || slot.key == key
        || slot.age() != age_
        || depth >= static_cast<int>(slot.depth);
    if (!replace) return;

    slot.key = key;
    slot.move = move;
    // Scores clamp to int16_t. Search scores are bounded by
    // ±Search::INF_SCORE (32001) — well inside int16_t's range.
    slot.score = static_cast<std::int16_t>(score);
    slot.depth = static_cast<std::uint8_t>(depth);
    slot.flags = static_cast<std::uint8_t>(
        (static_cast<std::uint8_t>(bound) & 0b11u)
        | (static_cast<std::uint8_t>(age_ & 0b11'1111u) << 2));
}

} // namespace chesserazade

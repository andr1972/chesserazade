// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Persistent on-disk PGN index.
///
/// `index_games` (see `pgn_index.hpp`) is a cheap header-only
/// scan — great for a list-view but oblivious to the actual
/// chess content. This layer adds:
///
///   * A stable per-game hash (FNV-1a over canonical-SAN main
///     line), so the same game in two PGN files identifies
///     itself identically — useful for bookmark lookup,
///     cross-file deduplication, and per-player aggregation.
///
///   * Disk persistence: the computed records live next to the
///     PGN as `<stem>.idx.json`, keyed by the PGN's mtime.
///     Reopening a PGN with an up-to-date companion index is
///     instant; stale or missing indexes rebuild on demand.
///
/// Later commits will attach more fields to `GameRecord` as
/// richer detectors land (mate / stalemate end, underpromotion,
/// knight forks, material blunders, …). The JSON `schema`
/// field tracks format version so future changes can migrate.
#pragma once

#include <chesserazade/pgn_index.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chesserazade {

/// Canonical-SAN hash of a game's main line. Zero means
/// "hash was not computed" (e.g. the move parser gave up on
/// the game) — never returned by a successful build.
using GameHash = std::uint64_t;

/// A single game's metadata. Extends `PgnGameHeader` with a
/// content-stable hash.
struct GameRecord {
    PgnGameHeader header;
    GameHash      hash = 0;
};

/// The full index for one PGN file.
struct GameIndex {
    /// On-disk format version. Bump when the JSON layout
    /// changes in a non-additive way.
    int schema = 1;

    /// Unix epoch seconds of the PGN file's last modification
    /// at the time the index was built. The loader compares
    /// this against the live file; a mismatch triggers a
    /// rebuild.
    std::int64_t pgn_mtime = 0;

    std::vector<GameRecord> games;
};

/// Progress callback for `build_index`. `done` is the number of
/// games whose hash has been computed so far; `total` is the
/// count of games the header-only indexer found. Called on the
/// builder thread (same thread as the caller — the builder is
/// synchronous). Return value is ignored; to cancel, the caller
/// sets the `cancel` atomic.
using BuildProgressCb =
    std::function<void(std::size_t done, std::size_t total)>;

/// Build a full index from raw PGN text. Calls `progress` every
/// 50 games and at completion. If `cancel` flips to `true` mid-
/// build, returns whatever was built so far (the `games` vector
/// is truncated to the prefix that completed).
[[nodiscard]] GameIndex
build_index(std::string_view pgn_bytes,
            std::int64_t pgn_mtime,
            const BuildProgressCb& progress,
            const std::atomic<bool>& cancel);

/// Serialize an index to `path` as pretty-printed JSON.
/// Overwrites any existing file. Returns false on write error.
[[nodiscard]] bool save_index(const std::string& path,
                              const GameIndex& idx);

/// Read an index from `path`. Returns nullopt if the file is
/// missing, unreadable, malformed, or carries a newer
/// `schema` than this build supports.
[[nodiscard]] std::optional<GameIndex>
load_index(const std::string& path);

} // namespace chesserazade

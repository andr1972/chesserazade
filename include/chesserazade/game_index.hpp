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
#include <chesserazade/types.hpp>

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

/// How a game terminated on the board. Derived by replaying
/// the recorded moves and inspecting the final position's
/// legal-move set; independent of the PGN `Result` tag, which
/// only records who won / drew / adjourned, not *why*.
enum class EndKind : std::uint8_t {
    /// Replay failed, no moves, or the move parser gave up.
    Unknown = 0,
    /// No legal reply in the final position, and the side to
    /// move is in check → checkmate.
    Mate = 1,
    /// No legal reply in the final position, and the side to
    /// move is not in check → stalemate.
    Stalemate = 2,
    /// Legal replies still exist — the game ended by
    /// resignation, draw agreement, timeout, adjudication, or
    /// similar external cause.
    Other = 3,
};

/// A single non-queen promotion during the game. `ply` is
/// 1-based (white's first move = 1, black's first move = 2)
/// so it maps directly onto move numbers shown in the UI.
/// `piece` is what the pawn became — Knight / Bishop / Rook
/// (Queen is excluded by definition, which is what makes it
/// "under"-promotion).
struct UnderPromotion {
    int       ply = 0;
    PieceType piece = PieceType::None;
};

/// Sharp material drop against the mover, detected during
/// index build.
///
/// `loss_cp` is the **net** material the mover lost over
/// their move + opponent's replies, with the detection window
/// dynamically extended through chains of captures so that
/// immediate recaptures net against the initial loss.
///
/// `raw_piece` is the biggest single piece the mover lost
/// *in* that window, identified directly from the move's
/// `captured_piece` field rather than inferred from a cp
/// bucket. For Fischer's 17…Be6!! / 18.Bxb6 line: `loss_cp`
/// nets to rook-magnitude (queen-for-bishop) while
/// `raw_piece` = Queen, since a queen physically dropped.
/// `raw_loss_cp` is the cp value of that piece, stored for
/// sorting convenience.
///
/// `recovery_cp` tracks how much the mover recovered over a
/// 20-ply forward window. Measured as **endpoint minus
/// settle-point advantage**, not as a peak — so further
/// material losses after an initial recovery *subtract*, and
/// the final figure reflects net material swing through the
/// window. Clamped at zero on the low side.
///
/// `ply` is the sacrificing move (1-based).
struct MaterialSac {
    int       ply = 0;
    int       loss_cp = 0;
    PieceType raw_piece = PieceType::None;
    int       raw_loss_cp = 0;
    int       recovery_cp = 0;
};

/// A single game's metadata. Extends `PgnGameHeader` with a
/// content-stable hash and replay-derived events.
struct GameRecord {
    PgnGameHeader header;
    GameHash      hash     = 0;
    EndKind       end_kind = EndKind::Unknown;
    /// All non-queen promotions in play order. Empty for most
    /// games; classically interesting when present (knight
    /// promotion for a fork, bishop / rook to dodge a
    /// stalemate).
    std::vector<UnderPromotion> underpromotions;

    /// Plies on which a knight move delivered check *and*
    /// simultaneously attacked an opponent queen or rook — the
    /// classic "family check" / royal fork motif. 1-based
    /// plies. Empty for most games; a filter hit is usually
    /// worth looking at.
    std::vector<int> knight_fork_plies;

    /// All sacrifice / blunder events in play order.
    /// Recovery is checked over a 10-ply window; a sacrifice
    /// that recovered its full material cost is a "sound"
    /// sacrifice, while one that never got repaid is a
    /// blunder (or loss-leading sac that won by threat /
    /// checkmate pressure, not material).
    std::vector<MaterialSac> material_sacs;
};

/// The full index for one PGN file.
struct GameIndex {
    /// On-disk format version. Bump when the JSON layout
    /// changes in a non-additive way. Current: 8 (MaterialSac
    /// gained `raw_piece`; recovery metric changed).
    int schema = 8;

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

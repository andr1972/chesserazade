/// Bookmarks — user-saved pointers into specific positions of
/// specific games inside pgnmentor ZIP archives.
///
/// A bookmark is identified by:
///   - the ZIP filename it came from (so the loader knows which
///     archive to unzip / download),
///   - enough PGN-header fields to pick the right game inside
///     that archive (white, black, date, optional event / round
///     / eco for tie-breaking),
///   - a ply number into that game (0 = starting position).
///
/// User-facing metadata (label, free-text comment, single-level
/// folder) is stored alongside. Folders are plain strings; they
/// group bookmarks in the browse view but the persistent model
/// does not define or enforce a folder list — a folder exists
/// exactly as long as at least one bookmark names it.
///
/// Storage: `QStandardPaths::AppDataLocation/chesserazade/
/// bookmarks.json` (→ `~/.local/share/chesserazade/` on Linux).
/// Versioned; `schema_version` sits at 1 for the initial layout.
///
/// Matching (`resolve_game`) is fuzzy by design: a bookmark
/// saved with `date = "1972.07.11"` still loads from a cache
/// whose header reads `date = "1972.??.??"`; a `date = "1972"`
/// bookmark picks up any game in that year. Event / round / eco
/// kick in only when white+black+date aren't enough to isolate
/// a single game. The fallback contract is documented inline
/// on `resolve_game`.
#pragma once
#ifndef CHESSERAZADE_ANALYZER_BOOKMARKS_HPP
#define CHESSERAZADE_ANALYZER_BOOKMARKS_HPP

#include <chesserazade/pgn_index.hpp>

#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <vector>

namespace chesserazade::analyzer {

struct Bookmark {
    /// pgnmentor filename, e.g. `"Fischer.zip"`. Joined with
    /// `https://www.pgnmentor.com/players/` or the cache dir
    /// depending on whether the archive is local.
    QString zip;

    /// PGN Seven-Tag Roster fields captured at save time.
    /// Missing-at-save and missing-in-cache both stay as the
    /// empty string; `resolve_game` treats empty on either side
    /// as "any".
    QString white;
    QString black;
    QString date;     ///< "YYYY.MM.DD", "YYYY.??.??", "????.??.??", "YYYY", …
    QString event;    ///< optional tie-breaker
    QString round;    ///< optional tie-breaker
    QString eco;      ///< optional tie-breaker (unused until the indexer captures it)

    /// Position in the game. 0 = before white's first move.
    /// 2N-1 = after white's Nth move (black to move). 2N = after
    /// black's Nth move (white to move). The ply notation shown
    /// in the UI is rendered by `ply_to_notation`.
    int ply = 0;

    /// User-editable.
    QString label;    ///< short one-liner shown in the list.
    QString comment;  ///< free-form, editable after creation.
    QString folder;   ///< free text; "" = no folder.

    /// Epoch-milliseconds. Used as a secondary sort key so the
    /// most-recently-added bookmark surfaces at the top of each
    /// folder by default.
    std::int64_t created_ms = 0;
};

/// Render a ply as the move-number / side notation the user
/// types and reads. Examples:
///   ply 0  → "0"
///   ply 1  → "1 w"   (after white's 1st move)
///   ply 2  → "1 b"   (after black's 1st move)
///   ply 33 → "17 w"
/// The move column in the analyzer's game list uses the same
/// convention ("1…" for black's 1st move in SAN), so the
/// notation round-trips visually.
[[nodiscard]] QString ply_to_notation(int ply);

/// Inverse of `ply_to_notation`. Accepts "0", "N w", "N b"
/// (case-insensitive, whitespace-tolerant). Returns nullopt on
/// anything else — the caller decides whether that's an error
/// or a parse failure to surface.
[[nodiscard]] std::optional<int> notation_to_ply(const QString& s);

/// Full path to the bookmarks JSON. Normally
/// `~/.local/share/chesserazade/bookmarks.json`; on other
/// platforms whatever Qt picks for
/// `QStandardPaths::AppDataLocation`. The parent directory is
/// created on demand by `save_bookmarks`.
[[nodiscard]] QString bookmarks_file_path();

/// Read bookmarks from disk. A missing file returns an empty
/// vector (not an error — the user just hasn't saved any yet).
/// A malformed / unreadable file returns nullopt so the caller
/// can warn the user rather than silently drop their data.
[[nodiscard]] std::optional<std::vector<Bookmark>>
load_bookmarks();

/// Overwrite the bookmarks file with `all`. Creates the parent
/// directory if missing. Returns false on I/O failure.
[[nodiscard]] bool save_bookmarks(const std::vector<Bookmark>& all);

/// Find the game in `headers` that best matches `bm`. Algorithm:
///   1. Filter by white+black match (case-insensitive substring
///      either way, so "Fischer" matches "Fischer, Robert J.").
///      Empty on either side counts as match.
///   2. Among survivors, prefer those whose `date` agrees with
///      the bookmark's — a prefix match either way counts, so
///      "1972" in the bookmark matches "1972.07.11" in the
///      cache and vice-versa, and "????" treats unknown bits
///      as wildcards.
///   3. If still ambiguous, tie-break by event (substring), then
///      round (exact), then eco (exact when both sides have it).
///   4. If still multiple, return nullopt — the caller should
///      prompt the user with the remaining candidates.
///
/// Returns the index into `headers` of the winner, or nullopt
/// if zero or ≥2 candidates survive after tie-breaking.
[[nodiscard]] std::optional<std::size_t>
resolve_game(const Bookmark& bm,
             const std::vector<PgnGameHeader>& headers);

/// Sorted list of folders currently in use. Empty-folder
/// bookmarks don't contribute. Used by the "Add bookmark"
/// dialog's combobox for autocomplete.
[[nodiscard]] QStringList
folders_in_use(const std::vector<Bookmark>& all);

} // namespace chesserazade::analyzer

#endif

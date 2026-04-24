// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// PGN — Portable Game Notation I/O.
///
/// PGN is the standard archival format for a chess game:
///
/// ```
/// [Event "F/S Return Match"]
/// [Site "Belgrade, Serbia JUG"]
/// [Date "1992.11.04"]
/// [Round "29"]
/// [White "Fischer, Robert J."]
/// [Black "Spassky, Boris V."]
/// [Result "1/2-1/2"]
///
/// 1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 {This opening is called the Ruy Lopez.}
/// 4. Ba4 Nf6 5. O-O Be7 6. Re1 b5 7. Bb3 d6  1/2-1/2
/// ```
///
/// We commit to the *main-line* PGN subset for 0.4:
///   * Tag pairs are parsed and preserved.
///   * Standard Seven-Tag Roster (Event, Site, Date, Round, White,
///     Black, Result) is emitted in that order by the writer, even
///     when the input ordering differed.
///   * `[SetUp "1"]` + `[FEN "<fen>"]` set the starting position.
///   * Move text: SAN moves (via `san.hpp`), numbered turn indicators
///     (`12.`, `12...`), comments (`{…}` and `;…\n`), NAGs (`$1`,
///     `!`, `?`, `!!`, `??`, `!?`, `?!`) — all tolerated on input,
///     and none emitted by the writer beyond the moves themselves.
///   * Variations `(…)` are **skipped** on input, never emitted on
///     output. HANDOFF §9 0.4 says they are optional.
///   * Game termination `1-0`, `0-1`, `1/2-1/2`, `*` — required.
///
/// Reference:
///   * Standard PGN spec: https://www.chessclub.com/help/PGN-spec
///   * https://en.wikipedia.org/wiki/Portable_Game_Notation
#pragma once

#include <chesserazade/move.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chesserazade {

class Game;

/// The canonical Seven-Tag Roster (STR) required by the PGN
/// standard. Other tags are allowed in a PGN but the STR must be
/// present and appear first, in this order.
inline constexpr const char* PGN_STR_TAGS[] = {
    "Event", "Site", "Date", "Round", "White", "Black", "Result",
};

/// A parsed PGN game. The tags preserve the order in which they
/// appeared (except for STR emission order — see the writer). The
/// `moves` vector is resolved against the starting position, so it
/// can be fed straight into a `Game` to replay the line.
struct PgnGame {
    /// Tag pairs in appearance order. Duplicate keys are not
    /// deduplicated — the last occurrence wins if a caller looks
    /// them up, but the raw list is kept for faithful round-trips.
    std::vector<std::pair<std::string, std::string>> tags;

    /// If the PGN carries `[SetUp "1"]` and `[FEN "…"]`, this holds
    /// that FEN; otherwise the standard starting position is used.
    std::optional<std::string> starting_fen;

    /// Moves in play order. `moves[i]` is legal in the position
    /// reached after `moves[0..i-1]`.
    std::vector<Move> moves;

    /// Termination marker from the move-text section. Always one
    /// of `"1-0"`, `"0-1"`, `"1/2-1/2"`, `"*"`.
    std::string termination = "*";

    /// Convenience accessor for a tag by key. Returns the *last*
    /// value for that key, or nullopt.
    [[nodiscard]] std::optional<std::string> tag(std::string_view key) const;
};

/// Recoverable PGN parse error. Message is user-facing.
struct PgnError {
    std::string message;
};

/// Parse a PGN document containing a single game. Returns the
/// parsed game or an error describing the first malformed token.
[[nodiscard]] std::expected<PgnGame, PgnError>
parse_pgn(std::string_view text);

/// Per-move annotation attached to an output PGN. One entry per
/// ply, in play order. Both fields are optional — an empty
/// `suffix` skips the NAG, an empty `comment` skips the `{…}`
/// block after the move.
struct MoveAnnotation {
    std::string suffix;   // "!", "?", "!!", "??", "!?", "?!" or ""
    std::string comment;  // free text for a `{…}` block, or ""
};

/// Serialize a `Game` plus a tag set into PGN. The STR tags
/// (Event / Site / Date / Round / White / Black / Result) are
/// emitted in the canonical order; any other tags follow in the
/// order they appear in `tags`. If the game's starting position
/// is non-standard, `[SetUp "1"]` and `[FEN "…"]` are inserted
/// automatically. Move text is wrapped at a soft limit of 80
/// columns, ending with the `termination` marker.
[[nodiscard]] std::string write_pgn(
    const Game& game,
    const std::vector<std::pair<std::string, std::string>>& tags,
    std::string_view termination);

/// Annotated variant: each ply optionally gets a suffix glyph
/// (appended to the SAN, e.g. `Nf3?!`) and/or a post-move
/// `{comment}` block. `annotations.size()` must equal
/// `game.moves().size()`; trailing entries can be empty if no
/// annotation is desired for those plies.
[[nodiscard]] std::string write_pgn(
    const Game& game,
    const std::vector<std::pair<std::string, std::string>>& tags,
    std::string_view termination,
    const std::vector<MoveAnnotation>& annotations);

} // namespace chesserazade

/// PGN indexer — cheap header-only extraction over a multi-game
/// PGN document.
///
/// `parse_pgn` (see `pgn.hpp`) parses a single game fully, moves
/// and all. For the 1.3 Qt analyzer we need to populate a game-
/// list widget over a 900-game Fischer.pgn (~550 KB) quickly:
/// only the Seven-Tag Roster values and byte offsets are needed
/// up front. Moves can then be parsed lazily, one game at a
/// time, when the user clicks a row.
///
/// `index_games` walks the PGN text, finds every game boundary
/// (a `[Event "..."]` line at column 0 following start-of-text
/// or a blank separator), reads tag pairs until the move
/// section begins, and records one `PgnGameHeader` per game.
/// Move text is **not** parsed. The indexer is forgiving — a
/// game with a malformed tag is recorded with whichever fields
/// could be read and indexing continues with the next game.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace chesserazade {

/// Header snapshot of a single game in a multi-game PGN.
/// Fields hold the STR tag values (empty string when absent).
/// `offset` is the byte index of the game's first character
/// in the source text; `length` spans from `offset` to just
/// past the last character attributed to this game (including
/// its termination token, if any).
struct PgnGameHeader {
    std::string event;
    std::string site;
    std::string date;
    std::string round;
    std::string white;
    std::string black;
    std::string result;
    /// Optional ECO code ("B20", "C42", …) — empty when the
    /// source PGN does not carry `[ECO "..."]`.
    std::string eco;
    /// Number of half-moves in the main line. Variations and
    /// comments are excluded. Zero when the game has no move
    /// text (some "result only" PGN exports). Counted by a
    /// single-pass tokenizer inside `index_games`, so it is
    /// still cheaper than full SAN parsing.
    int ply_count = 0;
    std::size_t offset = 0;
    std::size_t length = 0;
};

/// Index every game in `pgn`. Moves are skipped — this runs in
/// O(n) over the text with no board / SAN work. Returns an
/// empty vector on an input that carries no `[Event ...]`
/// header.
[[nodiscard]] std::vector<PgnGameHeader>
index_games(std::string_view pgn);

} // namespace chesserazade

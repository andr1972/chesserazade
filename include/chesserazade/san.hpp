/// SAN — Standard Algebraic Notation I/O.
///
/// SAN is the human-readable move notation used in printed chess
/// books and in PGN files: `e4`, `Nf3`, `O-O`, `exd5`, `Nbd7`,
/// `Qh4+`, `e8=Q#`. It is position-dependent: the same string can
/// mean different moves in different positions (it uses
/// disambiguators only when strictly needed), and writing SAN
/// requires the *pre-move* board to know which disambiguator is
/// minimal. UCI (`e2e4`, `g1f3`) is the position-independent
/// alternative we use in the move generator's debug output.
///
/// The parser is deliberately lenient:
///   * Trailing `+`, `#`, `!`, `?` annotations (including `!!`,
///     `??`, `!?`, `?!`) are tolerated and ignored — we compute
///     check/mate ourselves when we write, and we don't need the
///     user to type them.
///   * `0-0` / `0-0-0` are accepted as aliases for `O-O` / `O-O-O`
///     (many PGN files in the wild use digits).
///
/// The writer is strict:
///   * Castling is emitted as `O-O` / `O-O-O` (letters, not digits).
///   * Check is `+`, mate is `#`. No NAGs — those come from the
///     analyzer in 0.9.
///   * Disambiguation is minimal: file if that uniquely identifies
///     the piece among legal movers to the target; else rank; else
///     full square. This matches the PGN standard.
///
/// Reference: https://en.wikipedia.org/wiki/Algebraic_notation_(chess)
#pragma once

#include <chesserazade/move.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace chesserazade {

class Board;

/// Recoverable error from SAN parsing. Message is human-readable.
struct SanError {
    std::string message;
};

/// Parse `text` as a SAN move in the context of `board`. The move
/// is resolved by intersecting the parsed description with the
/// list of legal moves in `board` — if zero or more than one match,
/// the parse fails.
[[nodiscard]] std::expected<Move, SanError>
parse_san(Board& board, std::string_view text);

/// Write `m` as a SAN string, using `board_before` (the position
/// *before* the move) to compute minimal disambiguation and the
/// check / mate suffix. The caller must ensure `m` is a legal move
/// in `board_before`.
[[nodiscard]] std::string to_san(Board& board_before, const Move& m);

} // namespace chesserazade

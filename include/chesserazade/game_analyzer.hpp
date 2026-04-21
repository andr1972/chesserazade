/// GameAnalyzer — annotate every ply of a game with the
/// engine's verdict.
///
/// Given a `Game` (starting position + move history), `analyze`
/// walks the plies in order. At each ply it:
///
///   1. Searches the *pre-move* position at depth N; the
///      resulting score is the best the mover could achieve.
///   2. Applies the played move and searches the *post-move*
///      position at depth N−1, negating to bring the score back
///      into the mover's perspective.
///   3. Computes `loss = best_score − played_score` (>= 0 when
///      the played move is suboptimal) and translates it into
///      a PGN NAG:
///
///          loss < 50 cp    →   none
///          50–150          →   "?!"   dubious
///          150–300         →   "?"    mistake
///          > 300           →   "??"   blunder
///
///      Mate swings (the engine saw mate but the mover missed it,
///      or walked into being mated) are always a blunder.
///
///   4. Records the engine's recommended move and principal
///      variation for the pre-move position. The CLI can emit
///      this as a `{best was Nf3 (+120cp), line: 1…}` comment.
///
/// Reference: https://en.wikipedia.org/wiki/Numeric_Annotation_Glyphs
#pragma once

#include <chesserazade/move.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chesserazade {

class Game;
class TranspositionTable;

/// Centipawn loss thresholds and their NAG suffixes. The bands
/// match common analyzer presets (Lichess, Chess.com). Users can
/// override via `AnalyzeOptions::dubious_cp_loss` etc. if their
/// taste differs.
struct NagThresholds {
    int dubious_cp_loss  = 50;   // loss ≥ this → "?!"
    int mistake_cp_loss  = 150;  // loss ≥ this → "?"
    int blunder_cp_loss  = 300;  // loss ≥ this → "??"
};

/// Per-ply analysis result.
struct MoveAnalysis {
    Move played{};                  // the move that was actually played
    Move best{};                    // the engine's top choice at this ply

    /// Engine's best score from the mover's perspective. A
    /// mate-score (`Search::is_mate_score`) represents a forced
    /// mate; the CLI formats this as "mate in N" / "mated in N".
    int best_score = 0;

    /// Score the played move actually achieves, again from the
    /// mover's perspective. By definition `played_score <=
    /// best_score` up to search noise.
    int played_score = 0;

    /// Principal variation *from the pre-move position* — begins
    /// with `best`. If the mover played the engine's best move,
    /// this equals the PV that includes the played move.
    std::vector<Move> best_line;

    /// PGN suffix annotation ("", "?!", "?", "??", "!" ...).
    /// 0.9 emits only negative annotations; positive ones (!)
    /// require recognising *non-obvious* good moves, which is an
    /// independent heuristic we've deferred.
    std::string nag_suffix;

    /// Human-readable comment the analyzer suggests attaching to
    /// this move in annotated PGN output. Empty when the move
    /// was accurate.
    std::string comment;
};

struct GameAnalysis {
    std::vector<MoveAnalysis> plies;
};

struct AnalyzeOptions {
    int depth = 10;
    NagThresholds nags{};

    /// Pre-allocated TT. Nullptr = analyzer allocates its own.
    /// Sharing one across analyze calls speeds up long games
    /// because positions late in the game often transpose with
    /// positions analysed earlier.
    TranspositionTable* tt = nullptr;
};

class GameAnalyzer {
public:
    [[nodiscard]] static GameAnalysis analyze(const Game& game,
                                              const AnalyzeOptions& opts);

    /// Convenience overload: analyze at the given fixed depth
    /// with default NAG thresholds and a fresh TT.
    [[nodiscard]] static GameAnalysis analyze(const Game& game, int depth);

    GameAnalyzer() = delete;
};

} // namespace chesserazade

// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include <chesserazade/game_analyzer.hpp>

#include "board/board8x8_mailbox.hpp"

#include <chesserazade/board.hpp>
#include <chesserazade/fen.hpp>
#include <chesserazade/game.hpp>
#include <chesserazade/san.hpp>
#include <chesserazade/search.hpp>
#include <chesserazade/transposition_table.hpp>

#include <cstddef>
#include <sstream>
#include <string>

namespace chesserazade {

namespace {

/// Re-replay the game's move history onto a fresh board. We do
/// not borrow `game.current_position()` because the analyzer
/// needs to walk from the start, pausing at each ply to analyze
/// the pre-move position.
Board8x8Mailbox replay_from_start(const Game& game) {
    auto start_fen = serialize_fen(game.starting_position());
    auto r = Board8x8Mailbox::from_fen(start_fen);
    // The starting FEN came from a live Game, so this cannot fail.
    return *r;
}

/// Classify a centipawn loss (or mate swing) into the PGN NAG
/// suffix a human annotator would write. Returns "" for moves
/// that are within the "no comment" band.
[[nodiscard]] std::string classify_nag(int loss, int best_score,
                                       int played_score,
                                       const NagThresholds& t) {
    // Mate swings dominate any centipawn band. "Had a forced mate
    // and didn't play it" and "walked into a forced mate" are
    // always blunders.
    const bool had_mate = Search::is_mate_score(best_score)
                          && best_score > 0;
    const bool missed_mate =
        had_mate && !(Search::is_mate_score(played_score)
                      && played_score > 0);
    const bool walked_into_mate =
        Search::is_mate_score(played_score) && played_score < 0
        && !(Search::is_mate_score(best_score) && best_score < 0);

    if (missed_mate || walked_into_mate) return "??";

    if (loss >= t.blunder_cp_loss) return "??";
    if (loss >= t.mistake_cp_loss) return "?";
    if (loss >= t.dubious_cp_loss) return "?!";
    return "";
}

/// Format the verbose {…} comment the analyzer attaches to a
/// non-accurate move. Shows the engine's preferred move (in
/// SAN) and its score. If the mover played the engine's top
/// move, we skip the comment — the move is its own annotation.
[[nodiscard]] std::string format_comment(const MoveAnalysis& a,
                                         Board& pre_move_board) {
    if (a.nag_suffix.empty()) return "";

    std::ostringstream os;
    os << "best was " << to_san(pre_move_board, a.best);

    if (Search::is_mate_score(a.best_score)) {
        const int p = Search::plies_to_mate(a.best_score);
        const int n = (p > 0 ? p : -p + 1) / 2 + ((p > 0 ? p : -p) % 2);
        if (p > 0) os << " (mate in " << n << ")";
        else       os << " (mated in " << n << ")";
    } else {
        os << " (" << (a.best_score >= 0 ? "+" : "")
           << a.best_score << "cp)";
    }
    return os.str();
}

} // namespace

GameAnalysis GameAnalyzer::analyze(const Game& game,
                                   const AnalyzeOptions& opts) {
    GameAnalysis out;

    Board8x8Mailbox board = replay_from_start(game);

    TranspositionTable local_tt;
    TranspositionTable* tt = opts.tt != nullptr ? opts.tt : &local_tt;

    const int depth = opts.depth > 0 ? opts.depth : 1;

    out.plies.reserve(game.moves().size());
    for (std::size_t i = 0; i < game.moves().size(); ++i) {
        const Move played = game.moves()[i];

        // 1. Engine's verdict on the pre-move position.
        SearchLimits l_best;
        l_best.max_depth = depth;
        SearchResult best_r = Search::find_best(board, l_best, tt);

        // 2. Engine's verdict on the position AFTER the played
        //    move, negated to stay in the mover's perspective.
        Board8x8Mailbox after = board;
        after.make_move(played);
        SearchLimits l_played;
        l_played.max_depth = depth > 1 ? depth - 1 : 1;
        SearchResult played_r = Search::find_best(after, l_played, tt);
        const int played_score = -played_r.score;

        MoveAnalysis a;
        a.played = played;
        a.best = best_r.best_move;
        a.best_score = best_r.score;
        a.played_score = played_score;
        a.best_line = std::move(best_r.principal_variation);

        // If the mover played the engine's best move, nothing to
        // flag — loss is zero (or near zero from search noise).
        if (played == a.best) {
            out.plies.push_back(std::move(a));
            board.make_move(played);
            continue;
        }

        const int loss = a.best_score - a.played_score;
        a.nag_suffix = classify_nag(loss, a.best_score, a.played_score,
                                    opts.nags);
        a.comment = format_comment(a, board);

        out.plies.push_back(std::move(a));
        board.make_move(played);
    }

    return out;
}

GameAnalysis GameAnalyzer::analyze(const Game& game, int depth) {
    AnalyzeOptions opts;
    opts.depth = depth;
    return analyze(game, opts);
}

} // namespace chesserazade

// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Search — alpha-beta negamax with iterative deepening and
/// optional time / node limits.
///
/// This is the 0.6 search. The algorithm is still a recognizable
/// textbook negamax, but with two classical additions:
///
///   * **Alpha-beta pruning.** At each node we carry a `[alpha,
///     beta]` window. A child score that matches or beats `beta`
///     (a "beta cutoff") proves the opponent will never allow
///     this line, so we stop exploring siblings. Result: the
///     same best move as plain negamax, but the tree can be up
///     to an order of magnitude smaller.
///     See https://www.chessprogramming.org/Alpha-Beta
///
///   * **Iterative deepening.** Rather than jumping straight to
///     depth N, we call the search at depth 1, then 2, then 3,
///     …, up to the requested depth or until a limit fires.
///     The cost of the shallow iterations is dwarfed by the
///     final one, and we get an anytime property: if the search
///     is cut off, the best move from the last *completed* depth
///     is still available.
///     See https://www.chessprogramming.org/Iterative_Deepening
///
/// Scoring is unchanged from 0.5 (mate = `±(MATE_SCORE - ply)`,
/// stalemate = 0, evaluator otherwise). See `evaluator.hpp` for
/// the static eval, and `is_mate_score` / `plies_to_mate` below
/// for the interpretation helpers.
///
/// Not yet here (per HANDOFF): transposition table (0.7), move
/// ordering beyond trivial (0.8), quiescence (0.8).
#pragma once

#include <chesserazade/move.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

namespace chesserazade {

class Board;
class TranspositionTable;

/// Optional limits on a search. Any combination may be set; the
/// search stops at the earliest trigger. A default-constructed
/// `Limits` is effectively unlimited (depth cap is the only hard
/// stop — without it the search would recurse forever on a loop
/// position).
struct SearchLimits {
    /// Maximum depth in plies. The search iterates up to (and
    /// including) this depth. Clamped to `Search::MAX_DEPTH`.
    int max_depth = 32;

    /// Wall-clock budget. Zero means "no time limit". Checked
    /// periodically during the search (not on every node, to
    /// keep the check cheap).
    std::chrono::milliseconds time_budget{0};

    /// Soft cap on visited nodes. Zero means "no node limit".
    /// Checked on the same cadence as `time_budget`.
    std::uint64_t node_budget = 0;

    /// Turn α-β pruning off inside the main negamax and run
    /// plain minimax. At small depths this is the natural way
    /// to inspect the full game tree without cutoffs hiding
    /// branches; at realistic depths the tree explodes. TT
    /// bound returns (Lower / Upper) are bypassed in this
    /// mode because they would carry over α-β assumptions;
    /// Exact entries remain valid. Note: quiescence is *not*
    /// affected by this flag — a capture tree without α-β
    /// expands without bound (any positive-SEE recapture
    /// sequence keeps growing), so there is no useful
    /// "plain-minimax quiescence" mode to expose.
    bool disable_alpha_beta = false;

    /// Skip quiescence search at the horizon and return the
    /// static evaluator directly. Lets a caller see what the
    /// raw depth-N score looks like without the capture-chain
    /// follow-through that normally papers over the horizon
    /// effect.
    bool disable_quiescence = false;

    /// At the root (ply 0), give every child its own full
    /// α-β window instead of the shrinking one that improves
    /// as good siblings are found. Each root move is therefore
    /// exact — no fail-low clamping to the current best — at
    /// the cost of losing root-level pruning. Deeper nodes
    /// prune normally. Useful for the analyzer's tree view.
    bool root_full_window = false;

    /// Use the board's O(1) incrementally-maintained eval
    /// instead of the 64-square full scan. Mathematically
    /// equivalent; purely a speed toggle for measurement.
    bool use_incremental_eval = false;

    /// Late Move Reductions. Quiet moves past the first few in
    /// the move-ordered list are searched at a reduced depth
    /// (depth - 1 - R). If the reduced search beats `alpha`,
    /// the move is re-searched at full depth to confirm. The
    /// assumption — validated by MVV-LVA + killer ordering —
    /// is that most "late" quiet moves are worse than the
    /// already-found best, so a cheap shallow verification is
    /// enough to prove it. The reduced probe runs with the
    /// recorder detached (identically to null-move pruning),
    /// so the tree view only sees the confirmed re-search.
    /// See https://www.chessprogramming.org/Late_Move_Reductions
    bool enable_lmr = false;

    /// History heuristic for quiet-move ordering. A per-search
    /// `[color][from][to]` table is bumped by `depth^2` on a
    /// quiet move's β-cutoff; at the next node the same
    /// `(from, to)` pair sorts ahead of never-cut quiets.
    /// Off means every quiet move scores 0 — LMR will then
    /// reduce all late quiets uniformly, which is the 1.x
    /// behaviour before history was added.
    /// See https://www.chessprogramming.org/History_Heuristic
    bool enable_history = false;

    /// Optional external cancel — setting the pointed-to flag
    /// to `true` makes the search abort at the next budget
    /// check (same cadence as the time / node budgets). The
    /// analyzer's Break button wires into this.
    std::atomic<bool>* cancel = nullptr;

    /// Optional progress counter — Search stores the running
    /// node visit count into the pointed-to atomic at every
    /// budget-check boundary. The analyzer's live-progress
    /// label polls it; other callers can leave it null.
    std::atomic<std::uint64_t>* progress_nodes = nullptr;
};

/// Cumulative capture-value and check-giving counts along a
/// principal-variation branch. Captures are expressed in
/// centipawn units (pawn = 100, knight = 320, …; see
/// `piece_value` in `evaluator.hpp`); a count of 1 in
/// `checks_white` means the white side gave check once on
/// this branch.
///
/// Capture totals are maintained unconditionally — they come
/// straight from `Move::captured_piece` at zero cost. Check
/// counts require an after-move attack scan, so they are only
/// populated when a `TreeRecorder` is attached to the search.
struct BranchStats {
    int captures_white = 0;
    int captures_black = 0;
    int checks_white   = 0;
    int checks_black   = 0;
};

/// Observer invoked by `Search::find_best` at every node up to
/// `ply_cap()`. Exists to feed a tree-view in a GUI (the 1.3
/// Qt analyzer) without storing the full search tree —
/// `Search` still explores nodes beyond the cap, but it does
/// not call back into the recorder for them.
///
/// Event order per node: `enter(ply, move)` before the
/// recursive descent, `leave(ply, score, was_cutoff, stats)`
/// after. Root moves enter at `ply = 1`. Calls pair up: every
/// `enter` is matched by exactly one `leave`.
///
/// `was_cutoff` is `true` when this move's score caused its
/// parent to break out of the move loop (fail-high >= beta).
/// Moves that never ran because a sibling cut first are not
/// visible at all — by design, per 1.3.0 §"alpha-beta
/// cutoffs".
class TreeRecorder {
public:
    virtual ~TreeRecorder() = default;

    /// Maximum ply the recorder wants to see. Nodes deeper
    /// than this are searched but not reported.
    [[nodiscard]] virtual int ply_cap() const noexcept = 0;

    /// Called once at the start of each iterative-deepening
    /// iteration inside `find_best`, with the 1-based depth
    /// of that iteration. Recorders that build a tree should
    /// reset their state here — otherwise successive
    /// iterations accumulate duplicate root nodes (each ID
    /// iteration re-searches the whole tree from scratch).
    /// Default: no-op.
    virtual void begin_iteration(int depth) { (void)depth; }

    /// About to recurse into `move` at depth `ply`. `ply` is
    /// 1-based — root children are at ply 1.
    virtual void enter(int ply, const Move& move) = 0;

    /// Finished searching the subtree under the matching
    /// `enter`. `stats` aggregates captures / checks along the
    /// subtree's principal variation (this move down to its
    /// best-line leaf), including this move's own contribution.
    /// `remaining_depth` is the search-ply budget this node's
    /// own negamax ran with — useful for lazy re-search of a
    /// cap-bounded subtree (expanding "+more" in the UI).
    /// `alpha` / `beta` are the α-β window the main search was
    /// running with at this node; a lazy sub-search that wants
    /// to reproduce the same tree shape seeds its root with
    /// exactly these bounds. `subtree_nodes` is the number of
    /// negamax + quiescence nodes actually visited inside this
    /// subtree, including alpha-beta cuts and nodes below the
    /// recorder cap — the total "work" the engine did here.
    /// `gives_check` is true iff this move put the opposing
    /// king in check. Captures are derivable from the move
    /// itself, but "does this single move give check" requires
    /// a post-move board probe; we do it in the search once
    /// and pass the result through for the analyzer's filter
    /// dialog.
    /// `exact` is true iff the subtree rooted at this move's
    /// position visited all direct children in its own move
    /// loop (no β-cutoff at that node). When false, the
    /// reported `score` is a bound, not the true minimax
    /// value — the tree view renders it as "≤score" to
    /// distinguish it from a fully-resolved score.
    virtual void leave(int ply, int score, bool was_cutoff,
                       const BranchStats& stats,
                       int remaining_depth,
                       int alpha, int beta,
                       std::uint64_t subtree_nodes,
                       bool gives_check,
                       bool exact) = 0;
};

struct SearchResult {
    /// Best move at the root. Default-constructed if the position
    /// has no legal moves (mate or stalemate at the root itself).
    Move best_move{};

    /// Score in centipawns, from the side-to-move's perspective.
    /// Positive = good for the side to move. May be a mate score;
    /// use `Search::is_mate_score` / `Search::plies_to_mate` to
    /// interpret.
    int score = 0;

    /// The main line starting from `best_move`, one entry per
    /// ply. `principal_variation[0] == best_move`.
    std::vector<Move> principal_variation;

    /// Number of nodes visited. Under iterative deepening this
    /// is the *cumulative* count across all depths searched.
    std::uint64_t nodes = 0;

    /// Wall-clock time for the whole search (all depths).
    std::chrono::milliseconds elapsed{0};

    /// Deepest depth that *completed* — the result reflects this
    /// depth, not necessarily `limits.max_depth`. When a time or
    /// node limit cuts in, `completed_depth` is one less than
    /// the iteration we abandoned, but `best_move` is still the
    /// best move from the last fully completed iteration.
    int completed_depth = 0;

    /// Transposition-table statistics for this search. When no
    /// TT is supplied to `find_best`, these remain zero.
    std::uint64_t tt_probes = 0;
    std::uint64_t tt_hits   = 0;

    /// Captures and checks along the principal variation from
    /// the root through to the leaf. Capture values are always
    /// populated; `checks_white` / `checks_black` remain zero
    /// unless a `TreeRecorder` was attached to the search.
    BranchStats pv_stats;
};

class Search {
public:
    /// The "you are mated at this ply" sentinel. Fits in 16 bits.
    static constexpr int MATE_SCORE = 32000;

    /// Any positional score the evaluator produces must stay
    /// strictly below this. Used as the initial ±∞ of the
    /// alpha-beta window at the root.
    static constexpr int INF_SCORE = 32001;

    /// Maximum search depth this engine will accept. Bounds the
    /// on-stack triangular PV table.
    static constexpr int MAX_DEPTH = 32;

    /// Find the best move in `board`, iteratively deepening up
    /// to `depth`. `board` is mutated during the search and
    /// restored on return. Equivalent to calling the `Limits`
    /// overload with `{ depth, 0ms, 0 }`.
    [[nodiscard]] static SearchResult find_best(Board& board, int depth);

    /// Find the best move honoring the given limits. The search
    /// runs iterative deepening from depth 1 up to
    /// `limits.max_depth`; it stops before the next iteration
    /// when the time or node budget has been exhausted.
    [[nodiscard]] static SearchResult find_best(Board& board,
                                                const SearchLimits& limits);

    /// Find the best move using the given transposition table.
    /// The table is owned by the caller so it can be re-used
    /// across calls (iterative deepening already re-uses it
    /// across depths, but a REPL session might want to share one
    /// across user moves). Pass `nullptr` to search without a
    /// TT — identical behavior to the non-TT overloads.
    [[nodiscard]] static SearchResult find_best(Board& board,
                                                const SearchLimits& limits,
                                                TranspositionTable* tt);

    /// Find the best move with an attached `TreeRecorder`. The
    /// recorder is called at every node up to its `ply_cap()`.
    /// When `recorder != nullptr`, per-node check detection is
    /// enabled and `SearchResult::pv_stats.checks_*` are
    /// populated. Pass `nullptr` for behaviour identical to
    /// the three-argument overload.
    [[nodiscard]] static SearchResult find_best(Board& board,
                                                const SearchLimits& limits,
                                                TranspositionTable* tt,
                                                TreeRecorder* recorder);

    /// Same as the four-argument overload but starts the root
    /// α-β window at `alpha` / `beta` instead of the usual
    /// ±INF. The analyzer's lazy subtree expansion uses this
    /// to reproduce exactly the search conditions the main
    /// pass ran with at a cap-bounded leaf, so the grafted
    /// subtree matches in shape.
    [[nodiscard]] static SearchResult find_best(Board& board,
                                                const SearchLimits& limits,
                                                TranspositionTable* tt,
                                                TreeRecorder* recorder,
                                                int alpha, int beta);

    /// True if `score` encodes a forced mate (won or lost).
    [[nodiscard]] static constexpr bool is_mate_score(int score) noexcept {
        return score > MATE_SCORE - 1000 || score < -(MATE_SCORE - 1000);
    }

    /// If `score` is a mate score, return the signed number of
    /// plies to mate (positive = we win, negative = we lose).
    /// Returns 0 for non-mate scores.
    [[nodiscard]] static constexpr int plies_to_mate(int score) noexcept {
        if (!is_mate_score(score)) return 0;
        return score > 0 ? MATE_SCORE - score : -(MATE_SCORE + score);
    }

    Search() = delete;
};

} // namespace chesserazade

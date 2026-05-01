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

#include <chesserazade/board.hpp>
#include <chesserazade/move.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
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
    int max_depth = 128;

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

    /// Aspiration windows at the root. For iterations at depth
    /// ≥ 4 the root α-β window is seeded narrow around the
    /// previous iteration's score (± 50 cp) instead of ±INF;
    /// a fail-low / fail-high triggers a re-search with a
    /// geometrically widened window. Most iterations stay
    /// inside the narrow window so the root α-β is tight from
    /// the first move — a cheap ordering win. Orthogonal to
    /// NMP / LMR / history (applies only to the outer ID loop).
    /// See https://www.chessprogramming.org/Aspiration_Windows
    bool enable_aspiration = false;

    /// Principal Variation Search. The first move after sorting
    /// runs with the full `[α, β]` window; subsequent moves are
    /// probed with a zero-width window `[α, α+1]` — the probe
    /// only needs to prove "this move does not beat α". A probe
    /// that beats α triggers a re-search with the full window
    /// to pin down the exact score. With good ordering (TT +
    /// MVV-LVA + killers + history) most non-first moves fail
    /// the probe on the first try, saving the cost of a
    /// full-window search. Composable with LMR: when both fire
    /// the probe uses the reduced depth *and* the zero window.
    /// See https://www.chessprogramming.org/Principal_Variation_Search
    bool enable_pvs = false;

    /// Check extensions. A move that puts the opponent in
    /// check recurses at `depth` instead of `depth - 1`, so
    /// check lines are searched one ply deeper than the
    /// formal depth would suggest. Makes forced-mate
    /// sequences that run through a string of checks
    /// resolvable at a smaller nominal depth, and fixes the
    /// horizon effect where quiescence (captures only) stops
    /// short of a waiting checking tactic.
    /// See https://www.chessprogramming.org/Check_Extensions
    bool enable_check_ext = false;

    /// Verification re-search for null-move pruning. When the
    /// regular NMP fails high at depth ≥ NMP_VERIFY_MIN_DEPTH,
    /// a second search of the same reduced depth is run for our
    /// own side (NMP disabled for us until ply exceeds a derived
    /// threshold) to confirm the cutoff. Catches zugzwang
    /// positions that vanilla NMP would mishandle. Ported from
    /// Stockfish classical (~+5 Elo at long TCs in their tests;
    /// expect less at chesserazade's strength).
    /// See https://www.chessprogramming.org/Null_Move_Pruning
    bool enable_nmp_verify = false;

    /// Forward futility pruning. At low depths in non-PV-nodes, skip
    /// quiet moves whose static eval + margin can't reach α. The
    /// margin grows with depth (a deeper search has more wiggle
    /// room to recover); captures / promotions / checks are exempt.
    /// See https://www.chessprogramming.org/Futility_Pruning
    bool enable_futility = false;

    /// Reverse futility / static null move pruning — at shallow
    /// depth in non-PV-nodes, if static_eval already exceeds β by
    /// a depth-scaled margin, return static_eval. Mirror of forward
    /// futility: that one prunes when we're hopeless, this one
    /// prunes when we're already winning by enough that even the
    /// opponent's best response can't bring us below β.
    /// See https://www.chessprogramming.org/Reverse_Futility_Pruning
    bool enable_reverse_futility = false;

    /// Null-move-pruning reduction formula. Selects how aggressively
    /// the null search shortens depth — bigger R prunes more (faster)
    /// but risks missing tactical refutations.
    ///   Off              — disable NMP entirely (baseline for A/B).
    ///   R4               — fixed R = 4. Very aggressive.
    ///   R3_PlusDepthDiv3 — R = 3 + depth/3. Scales with depth, the
    ///                      default for most modern engines.
    ///   R4_PlusDepthDiv4 — R = 4 + depth/4. More aggressive shallow,
    ///                      similar deep growth as the above.
    enum class NmpMode {
        Off,
        R4,
        R3_PlusDepthDiv3,   // SF-classical-ish, very aggressive at depth.
        R4_PlusDepthDiv4,   // Even more aggressive shallow.
        R3_PlusDepthDiv4,   // Gentler scaling — null-search depth ≥ 5 from d=10.
        R2_PlusDepthDiv3,   // Same shape as above at d=10, lower at d≤6.
    };
    NmpMode nmp_mode = NmpMode::R3_PlusDepthDiv4;

    /// Late-Move-Reduction reduction-amount policy. Picks how much
    /// to shorten the depth on a late quiet move before its probe.
    /// Re-search on probe-fail-high keeps every choice safe — the
    /// trade-off is purely 'cheaper probes vs. more re-searches'.
    ///   Off                  — disable LMR.
    ///   Constant1            — R = 1 always (current default).
    ///   LogDepthLogIndex     — Stockfish-style log(d)·log(i)/2.
    ///   DepthDiv4LogIdxHalf  — (d/4)·log(i)/2; matches SF near
    ///                          d=12, gentler shallow, deeper at
    ///                          high depth.
    ///   DepthDiv4LogIndex    — (d/4)·log(i); aggressive, biggest
    ///                          single-move reduction in the set.
    enum class LmrMode {
        Off,
        Constant1,
        LogDepthLogIndex,
        DepthDiv4LogIdxHalf,
        DepthDiv4LogIndex,
    };
    LmrMode lmr_mode = LmrMode::Constant1;

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

    /// Contempt: centipawns subtracted from the side-to-move's
    /// score whenever search returns a draw verdict (in-tree
    /// repetition or 50-move rule). With `contempt_cp = 20` and
    /// the engine's static eval at +200, a drawn line scores
    /// -20 (much worse than +200 → engine avoids the draw); at
    /// -200 the same draw scores -20 (much better than -200 →
    /// engine seeks the draw). The asymmetry — winners avoid
    /// repetitions, losers run for them — falls out automatically
    /// from a single signed value. Default 0 = "draw is exactly
    /// 0", same behaviour as before contempt was added.
    int contempt_cp = 0;

    /// Game-history zobrist keys reaching back from before the
    /// search root. The search treats any in-tree position whose
    /// zobrist matches an entry here OR an earlier entry on the
    /// current search path as a *repetition draw* and returns the
    /// draw score immediately — the same heuristic top engines
    /// use ("a single in-search repetition is a forced draw,
    /// because the opponent can always retread the loop").
    /// Empty span (the default) means "no history" — fine for
    /// puzzle-solving, but a UCI-driven game should populate this
    /// from the move stack so `e1 → e2 → e1` mid-search is
    /// detectable. The span's storage must outlive `find_best`.
    std::span<const ZobristKey> position_history{};
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

    /// Null-move-pruning diagnostics. `nmp_entered` counts every
    /// time the NMP gate let the null search run; `nmp_failed_high`
    /// is the subset where the null search returned ≥ β (a tentative
    /// cutoff). `nmp_verify_attempts` is the further subset that
    /// triggered the verification re-search (only when
    /// `enable_nmp_verify` is set and depth ≥ NMP_VERIFY_MIN_DEPTH);
    /// `nmp_verify_passed` is the subset where verification confirmed
    /// the cutoff. Useful for spotting whether verification ever fires
    /// at a given TC — short searches often never reach depth 13.
    /// Four primary states (mirroring the original log markers):
    ///   nmp_rejected — gate predicate failed (no null search run)
    ///   nmp_entered  — gate passed, null search executed
    ///   nmp_verified — verify search confirmed the cutoff
    ///   nmp_aborted  — verify search aborted (time/cancel)
    /// `nmp_failed_high` / `nmp_verify_attempts` are intermediate
    /// counts for diagnosing why verify did/didn't trigger.
    std::uint64_t nmp_rejected = 0;
    std::uint64_t nmp_entered = 0;
    std::uint64_t nmp_failed_high = 0;
    std::uint64_t nmp_verify_attempts = 0;
    std::uint64_t nmp_verified = 0;
    std::uint64_t nmp_aborted = 0;

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
    /// on-stack triangular PV / killer tables and the in-search
    /// recursion cap in `negamax`. Sized generously so a long
    /// analysis (hours of think time, deep iterative deepening
    /// plus check / single-reply extensions) doesn't index off
    /// the end of the fixed-size arrays.
    ///
    /// Stockfish uses MAX_PLY = 246 — overkill for an educational
    /// engine; 128 gives roughly 4× the practical seldepth (~50)
    /// and the on-stack cost is bounded: PvTable is
    /// 128 × 128 × sizeof(Move) ≈ 100 KiB, comfortable for the
    /// default 8 MiB thread stack.
    static constexpr int MAX_DEPTH = 128;

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

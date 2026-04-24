// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `SolveWorker` — runs `Search::find_best` on a worker thread
/// so the Qt event loop keeps drawing while the engine thinks.
///
/// The engine core itself is single-threaded; we put it on a
/// `QThread` owned by the solve panel, not inside it. The
/// worker iterates depth 1..max, re-uses the same
/// `TranspositionTable` across iterations, and emits a
/// `progress` signal per completed depth. A `finished` signal
/// closes the flow with the final PV.
///
/// Interruption is not wired (per 1.3.0 scope decision); the
/// user waits for the budget to expire or kills the window.
/// The 1.3.8 tree view will reuse the same worker and pass a
/// `TreeRecorder` through.
#pragma once

#include "board/board8x8_mailbox.hpp"
#include "search_tree.hpp"

#include <chesserazade/search.hpp>

#include <QMetaType>
#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>

Q_DECLARE_METATYPE(chesserazade::analyzer::SearchTree)

namespace chesserazade::analyzer {

struct SolveBudget {
    enum class Kind { Depth, TimeMs, Nodes };
    Kind kind = Kind::TimeMs;
    int  depth = 6;
    int  time_ms = 5000;
    long long nodes = 1'000'000;
    int tree_cap = 3;  ///< Ply cap for the TreeRecorder tree.
    bool disable_alpha_beta = false;  ///< Run plain minimax.
    bool disable_quiescence = false;  ///< Skip capture follow-through.
    bool root_full_window   = false;  ///< Exact root-move scores.
    bool use_tt             = true;   ///< Consult the TT.
    bool use_incremental_eval = false; ///< O(1) cached eval.
    bool use_bitboard       = false;  ///< Search on BoardBitboard.
};

/// Lives on a worker thread once `moveToThread` has been called
/// on it. The panel connects to its signals (queued across the
/// thread boundary) and invokes `start` via `QMetaObject::
/// invokeMethod` so the engine runs on the worker's event loop.
class SolveWorker final : public QObject {
    Q_OBJECT
public:
    SolveWorker(Board8x8Mailbox start_position, SolveBudget budget,
                std::atomic<bool>* cancel,
                std::atomic<std::uint64_t>* progress_nodes,
                QObject* parent = nullptr);

    /// Tree of the last completed iteration. Only valid after
    /// `finished` has fired. Non-owning view into the worker's
    /// buffer — copy what you need before the worker is
    /// destroyed.
    [[nodiscard]] const SearchTree& search_tree() const noexcept
        { return tree_; }

public slots:
    void start();

signals:
    /// One signal per completed ID iteration. `pv_uci` is a
    /// space-separated list of UCI moves. `nodes` / `elapsed_ms`
    /// are the totals from the `find_best` call that produced
    /// this iteration — useful for plotting per-depth cost in
    /// the progress log.
    void progress(int depth, int score_cp,
                  const QString& pv_uci,
                  int captures_white, int captures_black,
                  int checks_white,   int checks_black,
                  quint64 nodes, qint64 elapsed_ms);

    /// Fires right after `progress` for the same iteration.
    /// Carries a snapshot of the tree that iteration built —
    /// the panel swaps it in so the view updates after every
    /// completed depth. An iteration that aborts mid-search
    /// emits neither `progress` nor this signal, so the last
    /// snapshot visible to the user is always the one from the
    /// deepest *completed* iteration.
    void iteration_tree_ready(const chesserazade::analyzer::SearchTree& tree);

    /// Last signal of the flow. `best_uci` is the root move or
    /// "0000" on a terminal root. Score follows the same
    /// conventions as `SearchResult::score`.
    void finished(const QString& best_uci, int final_score,
                  int depth_reached, quint64 nodes,
                  qint64 elapsed_ms);

private:
    Board8x8Mailbox start_;
    SolveBudget     budget_;
    SearchTree      tree_;
    std::atomic<bool>* cancel_ = nullptr;
    std::atomic<std::uint64_t>* progress_nodes_ = nullptr;
};

} // namespace chesserazade::analyzer

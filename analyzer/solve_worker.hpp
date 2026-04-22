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

#include <chesserazade/search.hpp>

#include <QObject>
#include <QString>

namespace chesserazade::analyzer {

struct SolveBudget {
    enum class Kind { Depth, TimeMs, Nodes };
    Kind kind = Kind::TimeMs;
    int  depth = 6;
    int  time_ms = 5000;
    long long nodes = 1'000'000;
};

/// Lives on a worker thread once `moveToThread` has been called
/// on it. The panel connects to its signals (queued across the
/// thread boundary) and invokes `start` via `QMetaObject::
/// invokeMethod` so the engine runs on the worker's event loop.
class SolveWorker final : public QObject {
    Q_OBJECT
public:
    SolveWorker(Board8x8Mailbox start_position, SolveBudget budget,
                QObject* parent = nullptr);

public slots:
    void start();

signals:
    /// One signal per completed ID iteration. `pv_uci` is a
    /// space-separated list of UCI moves.
    void progress(int depth, int score_cp,
                  const QString& pv_uci,
                  int captures_white, int captures_black,
                  int checks_white,   int checks_black);

    /// Last signal of the flow. `best_uci` is the root move or
    /// "0000" on a terminal root. Score follows the same
    /// conventions as `SearchResult::score`.
    void finished(const QString& best_uci, int final_score,
                  int depth_reached, quint64 nodes,
                  qint64 elapsed_ms);

private:
    Board8x8Mailbox start_;
    SolveBudget     budget_;
};

} // namespace chesserazade::analyzer

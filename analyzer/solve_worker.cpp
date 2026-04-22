#include "solve_worker.hpp"

#include <chesserazade/move.hpp>
#include <chesserazade/transposition_table.hpp>

#include <QDateTime>

#include <chrono>

namespace chesserazade::analyzer {

SolveWorker::SolveWorker(Board8x8Mailbox start, SolveBudget budget,
                         QObject* parent)
    : QObject(parent), start_(std::move(start)), budget_(budget) {
    // Queued connections serialise `SearchTree` through Qt's
    // metatype system; register once on first use.
    qRegisterMetaType<SearchTree>("chesserazade::analyzer::SearchTree");
}

namespace {

[[nodiscard]] QString pv_to_uci(const std::vector<Move>& pv) {
    QString s;
    for (std::size_t i = 0; i < pv.size(); ++i) {
        if (i > 0) s += QLatin1Char(' ');
        s += QString::fromStdString(to_uci(pv[i]));
    }
    return s;
}

} // namespace

void SolveWorker::start() {
    using clk = std::chrono::steady_clock;
    const auto begin = clk::now();

    TranspositionTable tt;
    int max_depth = Search::MAX_DEPTH;
    long long budget_ms = 0;
    long long budget_nodes = 0;

    switch (budget_.kind) {
        case SolveBudget::Kind::Depth:
            max_depth = budget_.depth;
            break;
        case SolveBudget::Kind::TimeMs:
            budget_ms = budget_.time_ms;
            break;
        case SolveBudget::Kind::Nodes:
            budget_nodes = budget_.nodes;
            break;
    }
    if (max_depth < 1) max_depth = 1;
    if (max_depth > Search::MAX_DEPTH) max_depth = Search::MAX_DEPTH;

    SearchResult last;
    bool have_result = false;

    // Recorder is reset before every iteration so the final
    // tree in `tree_` reflects the deepest iteration that ran
    // to completion. We discard mid-iteration partial trees:
    // without a completed score they would lie about cutoffs.
    SearchTreeRecorder recorder(tree_, budget_.tree_cap);

    for (int d = 1; d <= max_depth; ++d) {
        SearchLimits lim;
        lim.max_depth = d;
        lim.disable_alpha_beta = budget_.disable_alpha_beta;
        lim.disable_quiescence = budget_.disable_quiescence;
        if (budget_ms > 0) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    clk::now() - begin).count();
            const long long remaining = budget_ms - elapsed;
            if (remaining <= 0) break;
            lim.time_budget = std::chrono::milliseconds{remaining};
        }
        if (budget_nodes > 0) {
            lim.node_budget = static_cast<std::uint64_t>(budget_nodes);
        }

        recorder.reset();
        Board8x8Mailbox work = start_;
        const SearchResult r = Search::find_best(work, lim, &tt, &recorder);

        if (r.completed_depth < d) {
            // Budget exhausted mid-iteration — the tree in
            // `tree_` has dangling `enter`s without matching
            // `leave`s. Don't emit it; the panel is still
            // holding the snapshot from the previous
            // completed iteration.
            if (!have_result) last = r;
            break;
        }

        last = r;
        have_result = true;
        emit progress(r.completed_depth, r.score,
                      pv_to_uci(r.principal_variation),
                      r.pv_stats.captures_white,
                      r.pv_stats.captures_black,
                      r.pv_stats.checks_white,
                      r.pv_stats.checks_black);

        // Copy on the worker thread, finalise SAN, mark the
        // PV so the view can bold it, then hand off. The next
        // iteration's recorder.reset() clears `tree_` without
        // disturbing this snapshot.
        SearchTree snapshot = tree_;
        snapshot.finalize_san(start_);
        snapshot.mark_best_subtrees();
        emit iteration_tree_ready(snapshot);

        if (Search::is_mate_score(r.score)) break;
    }

    const QString best_uci = (last.best_move.from == Square::None)
        ? QStringLiteral("0000")
        : QString::fromStdString(to_uci(last.best_move));
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clk::now() - begin).count();

    emit finished(best_uci, last.score,
                  last.completed_depth,
                  static_cast<quint64>(last.nodes),
                  static_cast<qint64>(elapsed_ms));
}

} // namespace chesserazade::analyzer

// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `SolvePanel` — budget picker + Run + info log for driving
/// `Search::find_best` on a worker thread.
///
/// Layout: a small board on the left shows the position being
/// solved (handed in by the caller — currently the
/// post-selected-ply board from `GameView`). The right side
/// has the budget controls, a Run button, and a log that
/// receives per-depth `info` lines.
///
/// 1.3.7 ships the flat version (info log); 1.3.8 swaps the
/// log for a QTreeView wired to `TreeRecorder`, and the board
/// starts updating when a tree row is clicked.
#pragma once

#include "board/board8x8_mailbox.hpp"
#include "search_tree.hpp"
#include "solve_worker.hpp"

#include <QWidget>

#include <atomic>
#include <chrono>
#include <cstdint>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QModelIndex;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QThread;
class QTimer;
class QTreeView;

namespace chesserazade::analyzer {

class BoardWidget;
class SearchTreeModel;

class SolvePanel final : public QWidget {
    Q_OBJECT
public:
    explicit SolvePanel(QWidget* parent = nullptr);
    ~SolvePanel() override;

    /// Prepare the panel for a fresh solve session. The board
    /// is shown on the left; the budget controls pick up the
    /// user's last choice.
    void set_position(const Board8x8Mailbox& board,
                      const QString& header_label);

signals:
    void back_requested();

protected:
    void keyPressEvent(QKeyEvent* e) override;

private:
    void on_run_clicked();
    void on_progress(int depth, int score_cp,
                     const QString& pv_uci,
                     int captures_white, int captures_black,
                     int checks_white,   int checks_black,
                     quint64 nodes, qint64 elapsed_ms);
    void on_finished(const QString& best_uci, int final_score,
                     int depth_reached, quint64 nodes,
                     qint64 elapsed_ms);
    void on_iteration_tree_ready(const SearchTree& tree);
    void on_tree_row_clicked(const QModelIndex& idx);
    void on_expansion_requested(int node_idx);
    void on_filter_clicked();
    void on_run_or_break_clicked();
    void on_progress_tick();
    void on_worker_thread_destroyed();
    void on_copy_fen_clicked();
    void on_set_fen_clicked();
    void on_tree_context_menu(const QPoint& pos);
    /// Re-apply the tree-search filter by selecting the first
    /// node whose SAN (or UCI fallback) matches the text in
    /// `tree_search_edit_`. Empty text clears the selection.
    /// Invoked on text change and after each re-populate of
    /// the tree — so the same query auto-selects the same
    /// move in every iteration of the same search, and again
    /// after re-running at a different depth/budget.
    void apply_tree_search();

    [[nodiscard]] SolveBudget current_budget() const;
    void set_running(bool running);
    void clear_log();
    void append_log(const QString& line);

    BoardWidget*  board_    = nullptr;
    QLabel*       header_   = nullptr;

    QRadioButton* rb_depth_ = nullptr;
    QRadioButton* rb_time_  = nullptr;
    QRadioButton* rb_nodes_ = nullptr;
    QButtonGroup* kind_group_ = nullptr;

    QSpinBox*     depth_spin_ = nullptr;
    QSpinBox*     time_spin_  = nullptr;
    QSpinBox*     nodes_spin_ = nullptr;
    QComboBox*    nodes_mult_ = nullptr;
    QSpinBox*     tree_cap_spin_ = nullptr;
    QCheckBox*    lazy_check_    = nullptr;
    QComboBox*    ab_mode_combo_ = nullptr;
    QCheckBox*    qs_check_      = nullptr;
    QCheckBox*    tt_check_      = nullptr;
    QCheckBox*    incr_eval_check_ = nullptr;
    QCheckBox*    bitboard_check_ = nullptr;
    QCheckBox*    lmr_check_      = nullptr;
    QCheckBox*    history_check_  = nullptr;
    QCheckBox*    asp_check_      = nullptr;
    QCheckBox*    pvs_check_      = nullptr;
    QCheckBox*    check_ext_check_ = nullptr;
    QComboBox*    nmp_mode_combo_  = nullptr;
    QComboBox*    lmr_mode_combo_  = nullptr;
    QCheckBox*    relative_score_check_ = nullptr;

    QPushButton*  run_btn_  = nullptr;
    QPushButton*  back_btn_ = nullptr;
    QLabel*       progress_label_ = nullptr;
    QPlainTextEdit* log_    = nullptr;
    QTreeView*    tree_view_ = nullptr;
    SearchTreeModel* tree_model_ = nullptr;
    QLineEdit*    tree_search_edit_ = nullptr;
    SearchTree    tree_;

    Board8x8Mailbox position_;
    /// Whichever board the BoardWidget is currently showing —
    /// kept in sync with it so Copy FEN grabs what the user
    /// sees (sentinel root, a tree-node descent, etc.).
    Board8x8Mailbox displayed_board_;
    QThread*      thread_ = nullptr;
    SolveWorker*  worker_ = nullptr;

    /// Cancellation + live-progress atomics, pointed into by
    /// the worker's SearchLimits. Lives on the panel so the
    /// UI can poll them independently of the worker's lifetime.
    std::atomic<bool> cancel_{false};
    std::atomic<std::uint64_t> progress_nodes_{0};
    std::chrono::steady_clock::time_point search_start_{};
    QTimer* progress_timer_ = nullptr;
};

} // namespace chesserazade::analyzer

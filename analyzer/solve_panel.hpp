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

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QModelIndex;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QThread;
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
                     int checks_white,   int checks_black);
    void on_finished(const QString& best_uci, int final_score,
                     int depth_reached, quint64 nodes,
                     qint64 elapsed_ms);
    void on_iteration_tree_ready(const SearchTree& tree);
    void on_tree_row_clicked(const QModelIndex& idx);
    void on_expansion_requested(int node_idx);
    void on_filter_clicked();

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
    QCheckBox*    ab_check_      = nullptr;
    QCheckBox*    qs_check_      = nullptr;

    QPushButton*  run_btn_  = nullptr;
    QPushButton*  back_btn_ = nullptr;
    QPlainTextEdit* log_    = nullptr;
    QTreeView*    tree_view_ = nullptr;
    SearchTreeModel* tree_model_ = nullptr;
    SearchTree    tree_;

    Board8x8Mailbox position_;
    QThread*      thread_ = nullptr;
    SolveWorker*  worker_ = nullptr;
};

} // namespace chesserazade::analyzer
